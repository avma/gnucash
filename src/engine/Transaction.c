/********************************************************************\
 * Transaction.c -- transaction & split implementation              *
 * Copyright (C) 1997 Robin D. Clark                                *
 * Copyright (C) 1997-2002 Linas Vepstas <linas@linas.org>          *
 * Copyright (C) 2000 Bill Gribble <grib@billgribble.com>           *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "AccountP.h"
#include "Backend.h"
#include "BackendP.h"
#include "GNCIdP.h"
#include "Group.h"
#include "TransactionP.h"
#include "TransLog.h"
#include "gnc-commodity.h"
#include "gnc-date.h"
#include "gnc-engine-util.h"
#include "gnc-engine.h"
#include "gnc-event-p.h"
#include "gnc-lot-p.h"
#include "gnc-lot.h"
#include "messages.h"
#include "qofbook.h"
#include "qofbook-p.h"
#include "qofobject.h"

#include "QueryObject.h"

/* 
 * The "force_double_entry" flag determines how 
 * the splits in a transaction will be balanced. 
 *
 * The following values have significance:
 * 0 -- anything goes
 * 1 -- The sum of all splits in a transaction will be
 *      forced to be zero, even if this requires the
 *      creation of additional splits.  Note that a split
 *      whose value is zero (e.g. a stock price) can exist
 *      by itself. Otherwise, all splits must come in at 
 *      least pairs.
 * 2 -- splits without parents will be forced into a
 *      lost & found account.  (Not implemented)
 */
int force_double_entry = 0;

const char *trans_notes_str = "notes";
const char *void_reason_str = "void-reason";
const char *void_time_str = "void-time";
const char *void_former_amt_str = "void-former-amount";
const char *void_former_val_str = "void-former-value";
const char *void_former_notes_str = "void-former-notes";

/* KVP entry for date-due value */
#define TRANS_DATE_DUE_KVP	"trans-date-due"
#define TRANS_TXN_TYPE_KVP	"trans-txn-type"
#define TRANS_READ_ONLY_REASON	"trans-read-only"

#define PRICE_SIGFIGS 6

#define ISO_DATELENGTH 30 /* length of an iso 8601 date string.
                           * not sure, can't be bothered counting :) */

/* This static indicates the debugging module that this .o belongs to.  */
static short module = MOD_ENGINE;


G_INLINE_FUNC void check_open (Transaction *trans);
G_INLINE_FUNC void
check_open (Transaction *trans)
{
  if (trans && 0 >= trans->editlevel)
  {
    PERR ("transaction %p not open for editing\n", trans);
    PERR ("\t%s:%d \n", __FILE__, __LINE__);
  }
}

/********************************************************************\
 * xaccInitSplit
 * Initialize a Split structure
\********************************************************************/

static void
xaccInitSplit(Split * split, QofBook *book)
{
  /* fill in some sane defaults */
  split->acc         = NULL;
  split->parent      = NULL;
  split->lot         = NULL;

  split->action      = g_cache_insert(gnc_engine_get_string_cache(), "");
  split->memo        = g_cache_insert(gnc_engine_get_string_cache(), "");
  split->reconciled  = NREC;
  split->amount      = gnc_numeric_zero();
  split->value       = gnc_numeric_zero();

  split->date_reconciled.tv_sec  = 0;
  split->date_reconciled.tv_nsec = 0;

  split->balance             = gnc_numeric_zero();
  split->cleared_balance     = gnc_numeric_zero();
  split->reconciled_balance  = gnc_numeric_zero();

  split->kvp_data = kvp_frame_new();
  split->idata = 0;

  split->book = book;

  xaccGUIDNew (&split->guid, book);
  xaccStoreEntity(book->entity_table, split, &split->guid, GNC_ID_SPLIT);
}

/********************************************************************\
\********************************************************************/

Split *
xaccMallocSplit(QofBook *book)
{
  Split *split;
  g_return_val_if_fail (book, NULL);

  split = g_new (Split, 1);
  xaccInitSplit (split, book);

  return split;
}

/********************************************************************\
\********************************************************************/
/* This routine is not exposed externally, since it does weird things, 
 * like not really setting up the parent account correctly, and ditto 
 * the parent transaction.  This routine is prone to programmer error
 * if not used correctly.  It is used only by the edit-rollback code.
 * Don't get duped!
 */

static Split *
xaccDupeSplit (Split *s)
{
  Split *split = g_new0 (Split, 1);

  /* copy(!) the guid and entity table. The cloned split is *not* unique,
   * is a sick twisted clone that holds 'undo' information. */
  split->guid = s->guid;
  split->book = s->book;

  split->parent = s->parent;
  split->acc = s->acc;
  split->lot = s->lot;

  split->memo = g_cache_insert (gnc_engine_get_string_cache(), s->memo);
  split->action = g_cache_insert (gnc_engine_get_string_cache(), s->action);

  split->kvp_data = kvp_frame_copy (s->kvp_data);

  split->reconciled = s->reconciled;
  split->date_reconciled = s->date_reconciled;

  split->value = s->value;
  split->amount = s->amount;

  /* no need to futz with the balances;  these get wiped each time ... 
   * split->balance             = s->balance;
   * split->cleared_balance     = s->cleared_balance;
   * split->reconciled_balance  = s->reconciled_balance;
   */

  return split;
}

static Split *
xaccSplitClone (Split *s)
{
  Split *split = g_new0 (Split, 1);

  split->book                = s->book;
  split->parent              = NULL;
  split->memo                = g_cache_insert(gnc_engine_get_string_cache(), s->memo);
  split->action              = g_cache_insert(gnc_engine_get_string_cache(), s->action);
  split->kvp_data            = kvp_frame_copy(s->kvp_data);
  split->reconciled          = s->reconciled;
  split->date_reconciled     = s->date_reconciled;
  split->value               = s->value;
  split->amount              = s->amount;
  split->balance             = s->balance;
  split->cleared_balance     = s->cleared_balance;
  split->reconciled_balance  = s->reconciled_balance;
  split->idata               = 0;

  xaccGUIDNew(&split->guid, s->book);
  xaccStoreEntity(s->book->entity_table, split, &split->guid, GNC_ID_SPLIT);

  xaccAccountInsertSplit(s->acc, split);
  if (s->lot) {
    s->lot->splits = g_list_append (s->lot->splits, split);
    s->lot->is_closed = -1;
  }
  return split;
}

#ifdef DUMP_FUNCTIONS
static void
xaccSplitDump (Split *split, const char *tag)
{
  printf("  %s Split %p", tag, split);
  printf("    GUID:     %s\n", guid_to_string(&split->guid));
  printf("    Book:     %p\n", split->book);
  printf("    Account:  %p\n", split->acc);
  printf("    Lot:      %p\n", split->lot);
  printf("    Parent:   %p\n", split->parent);
  printf("    Memo:     %s\n", split->memo ? split->memo : "(null)");
  printf("    Action:   %s\n", split->action ? split->action : "(null)");
  printf("    KVP Data: %p\n", split->kvp_data);
  printf("    Recncld:  %c (date %s)\n", split->reconciled, gnc_print_date(split->date_reconciled));
  printf("    Value:    %s\n", gnc_numeric_to_string(split->value));
  printf("    Amount:   %s\n", gnc_numeric_to_string(split->amount));
  printf("    Balance:  %s\n", gnc_numeric_to_string(split->balance));
  printf("    CBalance: %s\n", gnc_numeric_to_string(split->cleared_balance));
  printf("    RBalance: %s\n", gnc_numeric_to_string(split->reconciled_balance));
  printf("    idata:    %x\n", split->idata);
}
#endif

/********************************************************************\
\********************************************************************/

void
xaccFreeSplit (Split *split)
{
  if (!split) return;

  kvp_frame_delete (split->kvp_data);

  g_cache_remove(gnc_engine_get_string_cache(), split->memo);
  g_cache_remove(gnc_engine_get_string_cache(), split->action);

  /* just in case someone looks up freed memory ... */
  split->memo        = NULL;
  split->action      = NULL;
  split->kvp_data    = NULL;
  split->reconciled  = NREC;
  split->amount      = gnc_numeric_zero();
  split->value       = gnc_numeric_zero();
  split->parent      = NULL;
  split->lot         = NULL;
  split->acc         = NULL;
  
  split->date_reconciled.tv_sec = 0;
  split->date_reconciled.tv_nsec = 0;

  g_free(split);
}

/*
 * Helper routine for xaccSplitEqual.
 */
static gboolean
xaccSplitEqualCheckBal (const char *tag, gnc_numeric a, gnc_numeric b)
{
  char *str_a, *str_b;

  if (gnc_numeric_equal (a, b))
    return TRUE;

  str_a = gnc_numeric_to_string (a);
  str_b = gnc_numeric_to_string (b);

  PWARN ("%sbalances differ: %s vs %s", tag, str_a, str_b);

  g_free (str_a);
  g_free (str_b);

  return FALSE;
}

/********************************************************************
 * xaccSplitEqual
 ********************************************************************/
gboolean
xaccSplitEqual(const Split *sa, const Split *sb,
               gboolean check_guids,
               gboolean check_balances,
               gboolean check_txn_splits)
{
  if (!sa && !sb) return TRUE;

  if (!sa || !sb)
  {
    PWARN ("one is NULL");
    return FALSE;
  }

  /* Huh? This test wasn't here before, but IMHO it should be queried
   * as a very first thing. cstim, 2002-12-07 */
  if (sa == sb) return TRUE;

  if(check_guids) {
    if(!guid_equal(&(sa->guid), &(sb->guid)))
    {
      PWARN ("GUIDs differ");
      return FALSE;
    }
  }

  /* Since these strings are cached we can just use pointer equality */
  if (sa->memo != sb->memo)
  {
    PWARN ("memos differ: (%p)%s vs (%p)%s",
	   sa->memo, sa->memo, sb->memo, sb->memo);
    return FALSE;
  }

  if (sa->action != sb->action)
  {
    PWARN ("actions differ: %s vs %s", sa->action, sb->action);
    return FALSE;
  }

  if (kvp_frame_compare(sa->kvp_data, sb->kvp_data) != 0)
  {
    char *frame_a;
    char *frame_b;

    frame_a = kvp_frame_to_string (sa->kvp_data);
    frame_b = kvp_frame_to_string (sb->kvp_data);

    PWARN ("kvp frames differ:\n%s\n\nvs\n\n%s", frame_a, frame_b);

    g_free (frame_a);
    g_free (frame_b);

    return FALSE;
  }

  if (sa->reconciled != sb->reconciled)
  {
    PWARN ("reconcile flags differ: %c vs %c", sa->reconciled, sb->reconciled);
    return FALSE;
  }

  if (timespec_cmp(&(sa->date_reconciled),
                   &(sb->date_reconciled)))
  {
    PWARN ("reconciled date differs");
    return FALSE;
  }

  if (!gnc_numeric_eq(sa->amount, sb->amount))
  {
    char *str_a;
    char *str_b;

    str_a = gnc_numeric_to_string (sa->amount);
    str_b = gnc_numeric_to_string (sb->amount);

    PWARN ("amounts differ: %s vs %s", str_a, str_b);

    g_free (str_a);
    g_free (str_b);

    return FALSE;
  }

  if (!gnc_numeric_eq(sa->value, sb->value))
  {
    char *str_a;
    char *str_b;

    str_a = gnc_numeric_to_string (sa->value);
    str_b = gnc_numeric_to_string (sb->value);

    PWARN ("values differ: %s vs %s", str_a, str_b);

    g_free (str_a);
    g_free (str_b);

    return FALSE;
  }

  if (check_balances) {
    if (!xaccSplitEqualCheckBal ("", sa->balance, sb->balance))
      return FALSE;
    if (!xaccSplitEqualCheckBal ("cleared ", sa->cleared_balance, sb->cleared_balance))
      return FALSE;
    if (!xaccSplitEqualCheckBal ("reconciled ", sa->reconciled_balance, sb->reconciled_balance))
      return FALSE;
  }

  if (!xaccTransEqual(sa->parent, sb->parent, check_guids, check_txn_splits,
		      check_balances, FALSE))
  {
    PWARN ("transactions differ");
    return FALSE;
  }

  return(TRUE);
}

/********************************************************************
 * Account funcs
 ********************************************************************/

Account *
xaccSplitGetAccount (const Split *s)
{
  if (!s) return NULL;
  return s->acc;
}

/********************************************************************\
\********************************************************************/

const GUID *
xaccSplitGetGUID (const Split *split)
{
  if (!split) return xaccGUIDNULL();
  return &split->guid;
}

GUID
xaccSplitReturnGUID (const Split *split)
{
  if (!split) return *xaccGUIDNULL();
  return split->guid;
}

/********************************************************************\
\********************************************************************/

void 
xaccSplitSetGUID (Split *split, const GUID *guid)
{
  if (!split || !guid) return;
  check_open (split->parent);
  xaccRemoveEntity(split->book->entity_table, &split->guid);
  split->guid = *guid;
  xaccStoreEntity(split->book->entity_table, split,
                  &split->guid, GNC_ID_SPLIT);
}

/********************************************************************\
\********************************************************************/

Split *
xaccSplitLookup (const GUID *guid, QofBook *book)
{
  if (!guid || !book) return NULL;
  return xaccLookupEntity(qof_book_get_entity_table (book),
                          guid, GNC_ID_SPLIT);
}

Split *
xaccSplitLookupDirect (GUID guid, QofBook *book)
{
  if (!book) return NULL;
  return xaccLookupEntity(qof_book_get_entity_table (book),
                          &guid, GNC_ID_SPLIT);
}

/********************************************************************\
\********************************************************************/

void
xaccConfigSetForceDoubleEntry (int force) 
{
   force_double_entry = force;
}

int
xaccConfigGetForceDoubleEntry (void) 
{
   return (force_double_entry);
}

/********************************************************************\
\********************************************************************/
/* routines for marking splits dirty, and for sending out change
 * events.  Note that we can't just mark-n-generate-event in one
 * step, since sometimes we need to mark things up before its suitable
 * to send out a change event.
 */

G_INLINE_FUNC void mark_split (Split *s);
G_INLINE_FUNC void mark_split (Split *s)
{
  Account *account = s->acc;

  if (account && !account->do_free)
  {
    account->balance_dirty = TRUE;
    account->sort_dirty = TRUE;
  }

  /* set dirty flag on lot too. */
  if (s->lot) s->lot->is_closed = -1;
}


G_INLINE_FUNC void mark_trans (Transaction *trans);
G_INLINE_FUNC void mark_trans (Transaction *trans)
{
  GList *node;

  for (node = trans->splits; node; node = node->next)
  {
    mark_split (node->data);
  }
}

G_INLINE_FUNC void gen_event (Split *split);
G_INLINE_FUNC void gen_event (Split *split)
{
  Account *account = split->acc;
  Transaction *trans;

  if (account)
  {
     xaccGroupMarkNotSaved (account->parent);
     gnc_engine_generate_event (&account->guid, GNC_EVENT_MODIFY);
  }

  trans = split->parent;
  if (trans)
  {
    gnc_engine_generate_event (&trans->guid, GNC_EVENT_MODIFY);
  }
}

G_INLINE_FUNC void gen_event_trans (Transaction *trans);
G_INLINE_FUNC void gen_event_trans (Transaction *trans)
{
  GList *node;

  for (node = trans->splits; node; node = node->next)
  {
    Account *account = ((Split *) (node->data)) -> acc;
    if (account)
      xaccGroupMarkNotSaved (account->parent);
  }

  gnc_engine_generate_event (&trans->guid, GNC_EVENT_MODIFY);
}

/********************************************************************\
\********************************************************************/

static int
get_currency_denom(const Split * s)
{
    if(!s)
    {
        return 0;
    }
    else if(!s->parent || !s->parent->common_currency)
    {
        return 100000;
    }
    else
    {
        return gnc_commodity_get_fraction (s->parent->common_currency);
    }
}

static int
get_commodity_denom(const Split * s) 
{
    if(!s)
    {
        return 0;
    }
    else if (NULL == s->acc)
    {
        return 100000;
    }
    else
    {
        return xaccAccountGetCommoditySCU(s->acc);
    }
}

/********************************************************************
 * xaccSplitGetSlots
 ********************************************************************/

kvp_frame * 
xaccSplitGetSlots (const Split * s)
{
  if(!s) return NULL;
  return(s->kvp_data);
}

void
xaccSplitSetSlots_nc(Split *s, kvp_frame *frm)
{
  g_return_if_fail(s);
  g_return_if_fail(frm);
  check_open (s->parent);

  if (s->kvp_data && (s->kvp_data != frm))
  {
    kvp_frame_delete(s->kvp_data);
  }

  s->kvp_data = frm;

  gen_event (s);
}

/********************************************************************\
\********************************************************************/

void 
DxaccSplitSetSharePriceAndAmount (Split *s, double price, double amt)
{
  if (!s) return;
  check_open (s->parent);

  s->amount = double_to_gnc_numeric(amt, get_commodity_denom(s),
                                    GNC_RND_ROUND);
  s->value  = double_to_gnc_numeric(price * amt, get_currency_denom(s),
                                    GNC_RND_ROUND);

  mark_split (s);
  gen_event (s);
}

void 
xaccSplitSetSharePriceAndAmount (Split *s, gnc_numeric price, 
                                 gnc_numeric amt)
{
  if (!s) return;
  check_open (s->parent);

  s->amount = gnc_numeric_convert(amt, get_commodity_denom(s), GNC_RND_ROUND);
  s->value  = gnc_numeric_mul(s->amount, price, 
                              get_currency_denom(s), GNC_RND_ROUND);

  mark_split (s);
  gen_event (s);
}

void 
DxaccSplitSetSharePrice (Split *s, double amt) 
{
  xaccSplitSetSharePrice
    (s, double_to_gnc_numeric(amt, GNC_DENOM_AUTO,
                              GNC_DENOM_SIGFIGS(PRICE_SIGFIGS) |
                              GNC_RND_ROUND));
}

void 
xaccSplitSetSharePrice (Split *s, gnc_numeric price) 
{
  if (!s) return;
  check_open (s->parent);

  s->value = gnc_numeric_mul(s->amount, price, get_currency_denom(s),
                             GNC_RND_ROUND);

  mark_split (s);
  gen_event (s);
}

void 
DxaccSplitSetShareAmount (Split *s, double damt) 
{
  gnc_numeric old_price;
  gnc_numeric amt = double_to_gnc_numeric(damt, get_commodity_denom(s), 
                                          GNC_RND_ROUND); 
  if (!s) return;
  check_open (s->parent);
  
  if(!gnc_numeric_zero_p(s->amount)) {
    old_price = gnc_numeric_div(s->value, s->amount, GNC_DENOM_AUTO,
                                GNC_DENOM_REDUCE);
  }
  else {
    old_price = gnc_numeric_create(1, 1);
  }

  s->amount = gnc_numeric_convert(amt, get_commodity_denom(s), 
                                  GNC_RND_NEVER);
  s->value  = gnc_numeric_mul(s->amount, old_price, 
                              get_currency_denom(s), GNC_RND_ROUND);

  mark_split (s);
  gen_event (s);
}

void 
DxaccSplitSetAmount (Split *s, double damt) 
{
  gnc_numeric amt = double_to_gnc_numeric(damt, 
                                          get_currency_denom(s), 
                                          GNC_RND_ROUND);
  if (!s) return;
  check_open (s->parent);

  s->amount = gnc_numeric_convert(amt, get_commodity_denom(s), GNC_RND_ROUND);

  mark_split (s);
  gen_event (s);
}

void 
xaccSplitSetAmount (Split *s, gnc_numeric amt) 
{
  if(!s) return;
  check_open (s->parent);

  s->amount = gnc_numeric_convert(amt, get_commodity_denom(s), GNC_RND_ROUND);

  mark_split (s);
  gen_event (s);
}

void 
DxaccSplitSetValue (Split *s, double damt) 
{
  gnc_numeric amt = double_to_gnc_numeric(damt, 
                                          get_currency_denom(s), 
                                          GNC_RND_ROUND);
  gnc_numeric old_price;
  if (!s) return;
  check_open (s->parent);

  if(!gnc_numeric_zero_p(s->amount)) {
    old_price = gnc_numeric_div(s->value, s->amount, GNC_DENOM_AUTO,
                                GNC_DENOM_REDUCE);
  }
  else {
    old_price = gnc_numeric_create(1, 1);
  }

  s->value = gnc_numeric_convert(amt, get_currency_denom(s), 
                                 GNC_RND_NEVER);

  if(!gnc_numeric_zero_p(old_price)) {
    s->amount = gnc_numeric_div(s->value, old_price, get_currency_denom(s),
                                GNC_RND_ROUND);
  }

  mark_split (s);
  gen_event (s);
}

void 
xaccSplitSetValue (Split *s, gnc_numeric amt) 
{
  if(!s) return;
  check_open (s->parent);

  s->value = gnc_numeric_convert(amt, get_currency_denom(s), GNC_RND_ROUND);

  mark_split (s);
  gen_event (s);
}

/********************************************************************\
\********************************************************************/

gnc_numeric 
xaccSplitGetBalance (const Split *s) 
{
   if (!s) return gnc_numeric_zero();
   return s->balance;
}

gnc_numeric 
xaccSplitGetClearedBalance (const Split *s) 
{
   if (!s) return gnc_numeric_zero();
   return s->cleared_balance;
}

gnc_numeric 
xaccSplitGetReconciledBalance (const Split *s)  
{
   if (!s) return gnc_numeric_zero();
   return s->reconciled_balance;
}

/********************************************************************\
 * xaccInitTransaction
 * Initialize a transaction structure
\********************************************************************/

static void
xaccInitTransaction (Transaction * trans, QofBook *book)
{
  /* Fill in some sane defaults */
  trans->num         = g_cache_insert(gnc_engine_get_string_cache(), "");
  trans->description = g_cache_insert(gnc_engine_get_string_cache(), "");

  trans->common_currency = NULL;
  trans->splits = NULL;

  trans->date_entered.tv_sec  = 0;
  trans->date_entered.tv_nsec = 0;

  trans->date_posted.tv_sec  = 0;
  trans->date_posted.tv_nsec = 0;

  trans->version = 0;
  trans->version_check = 0;
  trans->marker = 0;
  trans->editlevel = 0;
  trans->do_free = FALSE;
  trans->orig = NULL;

  trans->kvp_data = kvp_frame_new();
  trans->idata = 0;

  trans->book = book;

  xaccGUIDNew (&trans->guid, book);
  xaccStoreEntity (book->entity_table, trans, &trans->guid, GNC_ID_TRANS);
}

/********************************************************************\
\********************************************************************/

Transaction *
xaccMallocTransaction (QofBook *book)
{
  Transaction *trans;

  g_return_val_if_fail (book, NULL);

  trans = g_new(Transaction, 1);
  xaccInitTransaction (trans, book);
  gnc_engine_generate_event (&trans->guid, GNC_EVENT_CREATE);

  return trans;
}

#ifdef DUMP_FUNCTIONS
void
xaccTransDump (Transaction *trans, const char *tag)
{
  GList *node;

  printf("%s Trans %p", tag, trans);
  printf("    GUID:        %s\n", guid_to_string(&trans->guid));
  printf("    Book:        %p\n", trans->book);
  printf("    Entered:     %s\n", gnc_print_date(trans->date_entered));
  printf("    Posted:      %s\n", gnc_print_date(trans->date_posted));
  printf("    Num:         %s\n", trans->num ? trans->num : "(null)");
  printf("    Description: %s\n", trans->description ? trans->description : "(null)");
  printf("    KVP Data:    %p\n", trans->kvp_data);
  printf("    Currency:    %s\n", gnc_commodity_get_printname(trans->common_currency));
  printf("    version:     %x\n", trans->version);
  printf("    version_chk: %x\n", trans->version_check);
  printf("    editlevel:   %x\n", trans->editlevel);
  printf("    do_free:     %x\n", trans->do_free);
  printf("    orig:        %p\n", trans->orig);
  printf("    idata:       %x\n", trans->idata);
  printf("    splits:      ");
  for (node = trans->splits; node; node = node->next)
  {
    printf("%p ", node->data);
  }
  printf("\n");
  for (node = trans->splits; node; node = node->next)
  {
    xaccSplitDump(node->data, tag);
  }
  printf("\n");
}
#endif

QofBook *
xaccTransGetBook (const Transaction *trans)
{
  if (!trans) return NULL;
  return trans->book;
}

void
xaccTransSortSplits (Transaction *trans)
{
  GList *node, *new_list = NULL;
  Split *split;

  /* first debits */
  for (node = trans->splits; node; node = node->next) {
    split = node->data;
    if (gnc_numeric_negative_p (split->value))
      continue;
    new_list = g_list_append(new_list, split);
  }

  /* then credits */
  for (node = trans->splits; node; node = node->next) {
    split = node->data;
    if (!gnc_numeric_negative_p (split->value))
      continue;
    new_list = g_list_append(new_list, split);
  }

  /* install newly sorted list */
  g_list_free(trans->splits);
  trans->splits = new_list;
}


/********************************************************************\
\********************************************************************/
/* This routine is not exposed externally, since it does weird things, 
 * like not really owning the splits correctly, and other weirdnesses. 
 * This routine is prone to programmer snafu if not used correctly. 
 * It is used only by the edit-rollback code.
 */

Transaction *
xaccDupeTransaction (Transaction *t)
{
  Transaction *trans;
  GList *node;

  trans = g_new0 (Transaction, 1);

  trans->num         = g_cache_insert (gnc_engine_get_string_cache(), t->num);
  trans->description = g_cache_insert (gnc_engine_get_string_cache(), t->description);

  trans->kvp_data = kvp_frame_copy (t->kvp_data);

  trans->splits = g_list_copy (t->splits);
  for (node = trans->splits; node; node = node->next)
  {
    node->data = xaccDupeSplit (node->data);
  }

  trans->date_entered = t->date_entered;
  trans->date_posted = t->date_posted;

  trans->version = t->version;
  trans->editlevel = 0;
  trans->do_free = FALSE;
  trans->orig = NULL;

  trans->common_currency = t->common_currency;

  /* copy(!) the guid and entity table.  The cloned transaction is
   * *not* unique, it is a sick twisted clone that holds 'undo'
   * information. */
  trans->guid = t->guid;
  trans->book = t->book;

  return trans;
}

/*
 * Use this routine to externally duplicate a transaction.  It creates
 * a full fledged transaction with unique guid, splits, etc.
 */
Transaction *
xaccTransClone (Transaction *t)
{
  Transaction *trans;
  Split *split;
  GList *node;

  gnc_engine_suspend_events();
  trans = g_new0 (Transaction, 1);

  trans->book            = t->book;
  trans->date_entered    = t->date_entered;
  trans->date_posted     = t->date_posted;
  trans->num             = g_cache_insert (gnc_engine_get_string_cache(), t->num);
  trans->description     = g_cache_insert (gnc_engine_get_string_cache(), t->description);
  trans->kvp_data        = kvp_frame_copy (t->kvp_data);
  trans->common_currency = t->common_currency;
  trans->version       	 = t->version;
  trans->version_check 	 = t->version_check;

  trans->editlevel     	 = 0;
  trans->do_free       	 = FALSE;
  trans->orig          	 = NULL;
  trans->idata         	 = 0;

  xaccGUIDNew (&trans->guid, t->book);
  xaccStoreEntity (t->book->entity_table, trans, &trans->guid, GNC_ID_TRANS);

  xaccTransBeginEdit(trans);
  for (node = t->splits; node; node = node->next)
  {
    split = xaccSplitClone(node->data);
    split->parent = trans;
    trans->splits = g_list_append (trans->splits, split);
  }
  xaccTransCommitEdit(trans);
  gnc_engine_resume_events();

  return trans;
}


/********************************************************************\
\********************************************************************/

static void
xaccFreeTransaction (Transaction *trans)
{
  GList *node;

  if (!trans) return;

  ENTER ("addr=%p\n", trans);

  /* free up the destination splits */
  for (node = trans->splits; node; node = node->next)
    xaccFreeSplit (node->data);
  g_list_free (trans->splits);
  trans->splits = NULL;

  /* free up transaction strings */
  g_cache_remove(gnc_engine_get_string_cache(), trans->num);
  g_cache_remove(gnc_engine_get_string_cache(), trans->description);

  kvp_frame_delete (trans->kvp_data);

  /* just in case someone looks up freed memory ... */
  trans->num         = NULL;
  trans->description = NULL;
  trans->kvp_data    = NULL;

  trans->date_entered.tv_sec = 0;
  trans->date_entered.tv_nsec = 0;

  trans->date_posted.tv_sec = 0;
  trans->date_posted.tv_nsec = 0;

  trans->version = 0;
  trans->editlevel = 0;
  trans->do_free = FALSE;

  if (trans->orig)
  {
    xaccFreeTransaction (trans->orig);
    trans->orig = NULL;
  }

  g_free(trans);

  LEAVE ("addr=%p\n", trans);
}

/********************************************************************
 xaccTransEqual

 Compare two transactions for equality.  We don't pay any attention to
 rollback issues here, and we only care about equality of "permanent
 fields", basically the things that would survive a file save/load
 cycle.

 ********************************************************************/

/* return 0 when splits have equal guids */
static gint
compare_split_guids (gconstpointer a, gconstpointer b)
{
  Split *sa = (Split *) a;
  Split *sb = (Split *) b;

  if (sa == sb) return 0;
  if (!sa || !sb) return 1;

  return guid_compare (xaccSplitGetGUID (sa), xaccSplitGetGUID (sb));
}

gboolean
xaccTransEqual(const Transaction *ta, const Transaction *tb,
               gboolean check_guids,
               gboolean check_splits,
               gboolean check_balances,
               gboolean assume_ordered)
{

  if(!ta && !tb) return TRUE;

  if(!ta || !tb)
  {
    PWARN ("one is NULL");
    return FALSE;
  }

  if(check_guids) {
    if(!guid_equal(&(ta->guid), &(tb->guid)))
    {
      PWARN ("GUIDs differ");
      return FALSE;
    }
  }

  if(!gnc_commodity_equal(ta->common_currency, tb->common_currency))
  {
    PWARN ("commodities differ %s vs %s",
           gnc_commodity_get_unique_name (ta->common_currency),
           gnc_commodity_get_unique_name (tb->common_currency));
    return FALSE;
  }

  if(timespec_cmp(&(ta->date_entered), &(tb->date_entered)))
  {
    PWARN ("date entered differs");
    return FALSE;
  }

  if(timespec_cmp(&(ta->date_posted), &(tb->date_posted)))
  {
    PWARN ("date posted differs");
    return FALSE;
  }

  /* Since we use cached strings, we can just compare pointer
   * equality for num and description
   */
  if(ta->num != tb->num)
  {
    PWARN ("num differs: %s vs %s", ta->num, tb->num);
    return FALSE;
  }

  if(ta->description != tb->description)
  {
    PWARN ("descriptions differ: %s vs %s", ta->description, tb->description);
    return FALSE;
  }

  if(kvp_frame_compare(ta->kvp_data, tb->kvp_data) != 0)
  {
    char *frame_a;
    char *frame_b;

    frame_a = kvp_frame_to_string (ta->kvp_data);
    frame_b = kvp_frame_to_string (tb->kvp_data);

    PWARN ("kvp frames differ:\n%s\n\nvs\n\n%s", frame_a, frame_b);

    g_free (frame_a);
    g_free (frame_b);

    return FALSE;
  }

  if (check_splits)
  {
    if ((!ta->splits && tb->splits) || (!tb->splits && ta->splits))
    {
      PWARN ("only one has splits");
      return FALSE;
    }

    if (ta->splits && tb->splits)
    {
      GList *node_a, *node_b;

      for (node_a = ta->splits, node_b = tb->splits;
	   node_a;
	   node_a = node_a->next, node_b = node_b->next)
      {
        Split *split_a = node_a->data;
        Split *split_b;

        /* don't presume that the splits are in the same order */
	if (!assume_ordered)
	  node_b = g_list_find_custom (tb->splits, split_a, compare_split_guids);

        if (!node_b)
        {
          char *str_a;

          str_a = guid_to_string (xaccSplitGetGUID (split_a));

          PWARN ("first has split %s and second does not", str_a);

          g_free (str_a);

          return(FALSE);
        }

        split_b = node_b->data;

        if (!xaccSplitEqual (split_a, split_b, check_guids, check_balances, FALSE))
        {
          char *str_a, *str_b;

          str_a = guid_to_string (xaccSplitGetGUID (split_a));
          str_b = guid_to_string (xaccSplitGetGUID (split_b));

          PWARN ("splits %s and %s differ", str_a, str_b);

          g_free (str_a);
          g_free (str_b);

          return(FALSE);
        }
      }

      if (g_list_length (ta->splits) != g_list_length (tb->splits))
      {
        PWARN ("different number of splits");
        return(FALSE);
      }
    }
  }

  return(TRUE);
}

/********************************************************************
 * xaccTransGetSlots
 ********************************************************************/

kvp_frame * 
xaccTransGetSlots (const Transaction *t)
{
  if(!t) return NULL;
  return(t->kvp_data);
}

void
xaccTransSetSlots_nc (Transaction *t, kvp_frame *frm)
{
  g_return_if_fail(t);
  g_return_if_fail(frm);
  check_open (t);

  if (t->kvp_data && (t->kvp_data != frm))
  {
    kvp_frame_delete(t->kvp_data);
  }

  t->kvp_data = frm;

  gen_event_trans (t);
}

/********************************************************************\
\********************************************************************/

const GUID *
xaccTransGetGUID (const Transaction *trans)
{
  if (!trans) return xaccGUIDNULL();
  return &trans->guid;
}

GUID
xaccTransReturnGUID (const Transaction *trans)
{
  if (!trans) return *xaccGUIDNULL();
  return trans->guid;
}

/********************************************************************\
\********************************************************************/

void 
xaccTransSetGUID (Transaction *trans, const GUID *guid)
{
  if (!trans || !guid) return;
  xaccRemoveEntity(trans->book->entity_table, &trans->guid);
  trans->guid = *guid;
  xaccStoreEntity(trans->book->entity_table, trans,
                  &trans->guid, GNC_ID_TRANS);
}


/********************************************************************\
\********************************************************************/

Transaction *
xaccTransLookup (const GUID *guid, QofBook *book)
{
  if (!guid || !book) return NULL;
  return xaccLookupEntity (qof_book_get_entity_table (book),
                           guid, GNC_ID_TRANS);
}

Transaction *
xaccTransLookupDirect (GUID guid, QofBook *book)
{
  if (!book) return NULL;
  return xaccLookupEntity (qof_book_get_entity_table (book),
                           &guid, GNC_ID_TRANS);
}

/********************************************************************\
\********************************************************************/

void
DxaccSplitSetBaseValue (Split *s, double value, 
                       const gnc_commodity * base_currency)
{
  xaccSplitSetBaseValue(s, 
                        double_to_gnc_numeric(value, get_currency_denom(s), 
                                              GNC_RND_ROUND),
                        base_currency);
}

void
xaccSplitSetBaseValue (Split *s, gnc_numeric value, 
                       const gnc_commodity * base_currency)
{
  const gnc_commodity *currency;
  const gnc_commodity *commodity;

  if (!s) return;
  check_open (s->parent);

  /* Novice/casual users may not want or use the double entry
   * features of this engine. So, in particular, there may be the
   * occasional split without a parent account. Well, that's ok,
   * we'll just go with the flow. */
  if (NULL == s->acc) 
  {
    if (force_double_entry) 
    {
      PERR ("split must have a parent\n");
      g_return_if_fail (s->acc);
    } 
    else 
    { 
      s->value = value;
      s->amount = value;
    }
    mark_split (s);
    gen_event (s);
    return;
  }

  currency = xaccTransGetCurrency (s->parent);
  commodity = xaccAccountGetCommodity (s->acc);

  /* If the base_currency is the transaction's commodity ('currency'),
   * set the value.  If it's the account commodity, set the
   * amount. If both, set both. */
  if (gnc_commodity_equiv(currency, base_currency)) {
    if(gnc_commodity_equiv(commodity, base_currency)) {
      s->amount = gnc_numeric_convert(value,
                                      get_commodity_denom(s), 
                                      GNC_RND_NEVER);
    }
    s->value = gnc_numeric_convert(value, 
                                   get_currency_denom(s),
                                   GNC_RND_NEVER);
  }
  else if (gnc_commodity_equiv(commodity, base_currency)) {
    s->amount = gnc_numeric_convert(value, get_commodity_denom(s),
                                    GNC_RND_NEVER);
  }
  else if ((NULL==base_currency) && (0 == force_double_entry)) { 
    s->value = gnc_numeric_convert(value, get_currency_denom(s),
                                   GNC_RND_NEVER);
  }
  else {
    PERR ("inappropriate base currency %s "
          "given split currency=%s and commodity=%s\n",
          gnc_commodity_get_printname(base_currency), 
          gnc_commodity_get_printname(currency), 
          gnc_commodity_get_printname(commodity));
    return;
  }

  mark_split (s);
  gen_event (s);
}

gnc_numeric
xaccSplitGetBaseValue (const Split *s, 
		       const gnc_commodity * base_currency)
{
  const gnc_commodity *currency;
  const gnc_commodity *commodity;
  gnc_numeric value;

  if (!s) return gnc_numeric_zero();

  /* ahh -- users may not want or use the double entry 
   * features of this engine.  So, in particular, there
   * may be the occasional split without a parent account. 
   * Well, that's ok, we'll just go with the flow. 
   */
  if (NULL == s->acc) 
  {
    if (force_double_entry) 
    {
      g_return_val_if_fail (s->acc, gnc_numeric_zero ());
    } 
    else { 
      return s->value;
    }
  }

  currency = xaccTransGetCurrency (s->parent);
  commodity = xaccAccountGetCommodity (s->acc);

  /* be more precise -- the value depends on the currency we want it
   * expressed in.  */
  if (gnc_commodity_equiv(currency, base_currency)) {
    value = s->value;
  }
  else if (gnc_commodity_equiv(commodity, base_currency)) {
    value = s->amount;   
  }
  else if ((NULL == base_currency) && (0 == force_double_entry)) {
    value = s->value;
  }
  else {
    PERR ("inappropriate base currency %s "
          "given split currency=%s and commodity=%s\n",
          gnc_commodity_get_printname(base_currency), 
          gnc_commodity_get_printname(currency), 
          gnc_commodity_get_printname(commodity));
    return gnc_numeric_zero();
  }

  return value;
}

/********************************************************************\
\********************************************************************/

gnc_numeric
xaccSplitsComputeValue (GList *splits, Split * skip_me,
                        const gnc_commodity * base_currency)
{
  GList *node;
  gnc_numeric value;

  ENTER (" currency=%s", gnc_commodity_get_mnemonic (base_currency));
  value = gnc_numeric_zero();

  for (node = splits; node; node = node->next)
  {
    Split *s = node->data;

    if (s == skip_me) continue;

    /* ahh -- users may not want or use the double entry features of
     * this engine. So, in particular, there may be the occasional
     * split without a parent account. Well, that's ok, we'll just
     * go with the flow. */
    if (NULL == s->acc) 
    {
      if (force_double_entry) 
      {
        g_return_val_if_fail (s->acc, gnc_numeric_zero ());
      } 
      else 
      { 
        value = gnc_numeric_add(value, s->value,
                                GNC_DENOM_AUTO, GNC_DENOM_LCD);
      }
    }
    else if ((NULL == base_currency) && (0 == force_double_entry)) 
    {
      value = gnc_numeric_add(value, s->value,
                              GNC_DENOM_AUTO, GNC_DENOM_LCD);
    }
    else 
    {
      const gnc_commodity *currency;
      const gnc_commodity *commodity;

      currency = xaccTransGetCurrency (s->parent);
      commodity = xaccAccountGetCommodity (s->acc);

      /* OK, we've got a parent account, we've got currency, lets
       * behave like professionals now, instead of the shenanigans
       * above. Note that just because the currencies are equivalent
       * doesn't mean the denominators are the same! */
      if (base_currency &&
          gnc_commodity_equiv(currency, base_currency)) {
        value = gnc_numeric_add(value, s->value,
                                GNC_DENOM_AUTO, GNC_DENOM_LCD);
      }
      else if (base_currency && 
               gnc_commodity_equiv(commodity, base_currency)) {
        value = gnc_numeric_add(value, s->amount,
                                GNC_DENOM_AUTO, GNC_DENOM_LCD);
      }
      else {
        PERR ("inconsistent currencies\n"   
              "\tbase = '%s', curr='%s', sec='%s'\n",
               gnc_commodity_get_printname(base_currency),
               gnc_commodity_get_printname(currency),
               gnc_commodity_get_printname(commodity));
        g_return_val_if_fail (FALSE, gnc_numeric_zero ());
      }
    }
  }

  if (base_currency)
    return gnc_numeric_convert (value,
                                gnc_commodity_get_fraction (base_currency),
                                GNC_RND_ROUND);
  else
    return gnc_numeric_convert (value, GNC_DENOM_AUTO, GNC_DENOM_REDUCE);
  LEAVE (" ");
}

gnc_numeric
xaccTransGetImbalance (const Transaction * trans)
{
  if (!trans)
    return gnc_numeric_zero ();

  return xaccSplitsComputeValue (trans->splits, NULL, 
        trans->common_currency);
}

gnc_numeric
xaccTransGetAccountValue (const Transaction *trans, 
			  const Account *account)
{
  gnc_numeric total = gnc_numeric_zero ();
  GList *splits;

  if (!trans || !account)
    return total;

  for (splits = xaccTransGetSplitList (trans); splits; splits = splits->next)
  {
    Split *s = splits->data;
    Account *a = xaccSplitGetAccount (s);
    if (a == account)
      total = gnc_numeric_add (total, xaccSplitGetValue (s),
			       GNC_DENOM_AUTO, GNC_DENOM_LCD);
  }
  return total;
}

/********************************************************************\
\********************************************************************/

static gnc_commodity *
FindCommonExclSCurrency (SplitList *splits,
                         gnc_commodity * ra, gnc_commodity * rb,
                         Split *excl_split)
{
  GList *node;

  if (!splits) return NULL;

  for (node = splits; node; node = node->next)
  {
    Split *s = node->data;
    gnc_commodity * sa, * sb;

    if (s == excl_split) continue;

    /* Novice/casual users may not want or use the double entry 
     * features of this engine.   Because of this, there
     * may be the occasional split without a parent account. 
     * Well, that's ok,  we'll just go with the flow. 
     */
    if (force_double_entry)
    {
       g_return_val_if_fail (s->acc, NULL);
    }
    else if (NULL == s->acc)
    {
      continue;
    }

    sa = DxaccAccountGetCurrency (s->acc);
    sb = DxaccAccountGetSecurity (s->acc);

    if (ra && rb) {
       int aa = !gnc_commodity_equiv(ra,sa);
       int ab = !gnc_commodity_equiv(ra,sb);
       int ba = !gnc_commodity_equiv(rb,sa);
       int bb = !gnc_commodity_equiv(rb,sb);

       if ( (!aa) && bb) rb = NULL;
       else
       if ( (!ab) && ba) rb = NULL;
       else
       if ( (!ba) && ab) ra = NULL;
       else
       if ( (!bb) && aa) ra = NULL;
       else
       if ( aa && bb && ab && ba ) { ra = NULL; rb = NULL; }

       if (!ra) { ra = rb; rb = NULL; }
    }
    else
    if (ra && !rb) {
       int aa = !gnc_commodity_equiv(ra,sa);
       int ab = !gnc_commodity_equiv(ra,sb);
       if ( aa && ab ) ra = NULL;
    }

    if ((!ra) && (!rb)) return NULL;
  }

  return (ra);
}

/* This is the wrapper for those calls (i.e. the older ones) which
 * don't exclude one split from the splitlist when looking for a
 * common currency.  
 */
static gnc_commodity *
FindCommonCurrency (GList *splits, gnc_commodity * ra, gnc_commodity * rb)
{
  return FindCommonExclSCurrency(splits, ra, rb, NULL);
}

gnc_commodity *
xaccTransFindOldCommonCurrency (Transaction *trans, QofBook *book)
{
  gnc_commodity *ra, *rb, *retval;
  Split *split;

  if (!trans) return NULL;

  if (trans->splits == NULL) return NULL;

  g_return_val_if_fail (book, NULL);

  split = trans->splits->data;

  if (!split || NULL == split->acc) return NULL;

  ra = DxaccAccountGetCurrency (split->acc);
  rb = DxaccAccountGetSecurity (split->acc);

  retval = FindCommonCurrency (trans->splits, ra, rb);

  /* compare this value to what we think should be the 'right' value */
  if (!trans->common_currency)
  {
    trans->common_currency = retval;
  }
  else if (!gnc_commodity_equiv (retval,trans->common_currency))
  {
    PWARN ("expected common currency %s but found %s\n",
           gnc_commodity_get_unique_name (trans->common_currency),
           gnc_commodity_get_unique_name (retval));
  }

  if (NULL == retval)
  {
     /* in every situation I can think of, this routine should return 
      * common currency.  So make note of this ... */
     PWARN ("unable to find a common currency, and that is strange.");
  }

  return retval;
}

/********************************************************************\
\********************************************************************/
/* The new routine for setting the common currency */

gnc_commodity *
xaccTransGetCurrency (const Transaction *trans)
{
  if (!trans) return NULL;
  return trans->common_currency;
}

void
xaccTransSetCurrency (Transaction *trans, gnc_commodity *curr)
{
  GList *splits;
  gint fraction;

  if (!trans || !curr) return;
  check_open (trans);

  trans->common_currency = curr;
  fraction = gnc_commodity_get_fraction (curr);

  for (splits = trans->splits; splits; splits = splits->next)
  {
    Split *s = splits->data;
    s->value = gnc_numeric_convert(s->value, fraction, GNC_RND_ROUND);
  }

  mark_trans (trans);
  gen_event_trans (trans);
}

/********************************************************************\
\********************************************************************/

void
xaccTransBeginEdit (Transaction *trans)
{
   Backend *be;
   if (!trans) return;

   trans->editlevel ++;
   if (1 < trans->editlevel) return;

   if (0 >= trans->editlevel) 
   {
      PERR ("unbalanced call - resetting (was %d)", trans->editlevel);
      trans->editlevel = 1;
   }

   /* See if there's a backend.  If there is, invoke it. */
   be = xaccTransactionGetBackend (trans);
   if (be && be->begin)
      (be->begin) (be, GNC_ID_TRANS, trans);

   xaccOpenLog ();
   xaccTransWriteLog (trans, 'B');

   /* make a clone of the transaction; we will use this 
    * in case we need to roll-back the edit. 
    */
   trans->orig = xaccDupeTransaction (trans);
}

void
xaccTransCommitEdit (Transaction *trans)
{
   Split *split;
   Backend *be;
   const char *str;

   if (!trans) return;
   ENTER ("trans addr=%p", trans);

   trans->editlevel--;
   if (0 < trans->editlevel) return;

   if (0 > trans->editlevel)
   {
      PERR ("unbalanced call - resetting (was %d)", trans->editlevel);
      trans->editlevel = 0;
   }

   /* We increment this for the duration of the call
    * so other functions don't result in a recursive
    * call to xaccTransCommitEdit. */
   trans->editlevel++;

   /* At this point, we check to see if we have a valid transaction.
    * There are two possiblities:
    *   1) Its more or less OK, and needs a little cleanup
    *   2) It has zero splits, i.e. is meant to be destroyed.
    * We handle 1) immediately, and we call the backend before 
    * we go through with 2).
    */
   if (trans->splits && !(trans->do_free))
   {
      split = trans->splits->data;
 
      /* Try to get the sorting order lined up according to 
       * when the user typed things in.  */
      if (0 == trans->date_entered.tv_sec) {
         struct timeval tv;
         gettimeofday (&tv, NULL);
         trans->date_entered.tv_sec = tv.tv_sec;
         trans->date_entered.tv_nsec = 1000 * tv.tv_usec;
      }

      /* Alternately the transaction may have only one split in 
       * it, in which case that's OK if and only if the split has no 
       * value (i.e. is only recording a price). Otherwise, a single
       * split with a value can't possibly balance, thus violating the 
       * rules of double-entry, and that's way bogus. So create 
       * a matching opposite and place it either here (if force==1), 
       * or in some dummy account (if force==2).
       */
      if ((1 == force_double_entry) &&
          (NULL == g_list_nth(trans->splits, 1)) &&
          (!gnc_numeric_zero_p(split->amount))) 
      {
        Split * s = xaccMallocSplit(trans->book);
        xaccTransAppendSplit (trans, s);
        xaccAccountInsertSplit (s->acc, s);
        s->amount = gnc_numeric_neg(split->amount);
        s->value = gnc_numeric_neg(split->value);
        xaccSplitSetMemo (s, split->memo);
        xaccSplitSetAction (s, split->action);
      }
   }

   /* ------------------------------------------------- */
   /* OK, at this point, we are done making sure that 
    * we've got a validly constructed transaction.
    *
    * Next, sort the splits
    */
   xaccTransSortSplits(trans);

   /*
    * Next, we send it off to the back-end, to see if the
    * back-end will accept it.
    */

   /* See if there's a backend.  If there is, invoke it. */
   str = trans->description;
   str = str ? str : "(null)";
   PINFO ("descr is %s", str);

   be = xaccTransactionGetBackend (trans);
   if (be && be->commit) 
   {
      GNCBackendError errcode;

      /* clear errors */
      do {
        errcode = xaccBackendGetError (be);
      } while (ERR_BACKEND_NO_ERR != errcode);

      (be->commit) (be, GNC_ID_TRANS, trans);

      errcode = xaccBackendGetError (be);
      if (ERR_BACKEND_NO_ERR != errcode)
      {
         /* if the backend puked, then we must roll-back 
          * at this point, and let the user know that we failed.
          */
        if (ERR_BACKEND_MODIFIED == errcode)
        {
           PWARN_GUI(_("Another user has modified this transaction\n"
		       "\tjust a moment ago. Please look at their changes,\n"
		       "\tand try again, if needed.\n"));
        }

        /* push error back onto the stack */
        xaccBackendSetError (be, errcode);

        xaccTransRollbackEdit (trans);
        return;
      }
   }

   /* ------------------------------------------------- */
   if (!trans->splits || trans->do_free)
   {
      PINFO ("delete trans at addr=%p", trans);
      /* Make a log in the journal before destruction.  */
      xaccTransWriteLog (trans, 'D');
      xaccRemoveEntity(trans->book->entity_table, &trans->guid);
      xaccFreeTransaction (trans);
      return;
   }

   /* ------------------------------------------------- */
   /* Make sure all associated splits are in proper order
    * in their accounts with the correct balances. */
   xaccTransFixSplitDateOrder (trans);

   trans->do_free = FALSE;
   xaccTransWriteLog (trans, 'C');

   /* Get rid of the copy we made. We won't be rolling back, 
    * so we don't need it any more.  */
   xaccFreeTransaction (trans->orig);
   trans->orig = NULL;

   /* Put back to zero. */
   trans->editlevel--;

   LEAVE ("trans addr=%p\n", trans);
}

void
xaccTransRollbackEdit (Transaction *trans)
{
   Backend *be;
   Transaction *orig;
   int force_it=0, mismatch=0;
   int i;
   ENTER ("trans addr=%p\n", trans);

   if (!trans) return;
   trans->editlevel--;
   if (0 < trans->editlevel) return;

   if (0 > trans->editlevel)
   {
      PERR ("unbalanced call - resetting (was %d)", trans->editlevel);
      trans->editlevel = 0;
   }

   /* We increment this for the duration of the call
    * so other functions don't result in a recursive
    * call to xaccTransCommitEdit. */
   trans->editlevel++;

   /* copy the original values back in. */
   orig = trans->orig;

   /* If the transaction had been deleted before the rollback,
    * the guid would have been unlisted. Restore that */
   xaccStoreEntity(trans->book->entity_table, trans,
                   &trans->guid, GNC_ID_TRANS);

   trans->common_currency = orig->common_currency;

   g_cache_remove (gnc_engine_get_string_cache(), trans->num);
   trans->num = orig->num;
   orig->num = g_cache_insert(gnc_engine_get_string_cache(), "");

   g_cache_remove (gnc_engine_get_string_cache(), trans->description);
   trans->description = orig->description;
   orig->description = g_cache_insert(gnc_engine_get_string_cache(), "");

   kvp_frame_delete (trans->kvp_data);
   trans->kvp_data = orig->kvp_data;
   if (!trans->kvp_data)
     trans->kvp_data = kvp_frame_new ();
   orig->kvp_data = kvp_frame_new ();

   trans->date_entered = orig->date_entered;
   trans->date_posted = orig->date_posted;

   /* OK, we also have to restore the state of the splits.  Of course,
    * we could brute-force our way through this, and just clobber all of the
    * old splits, and insert all of the new splits, but this kind of brute
    * forcing will suck memory cycles.  So instead we'll try the gentle 
    * approach first.  Note that even in the gentle approach, the 
    * CheckDateOrder routine could be cpu-cyle brutal, so it maybe 
    * it could use some tuning.
    */
   if (trans->do_free)
   {
      force_it = 1;
      mismatch = 0;
   }
   else 
   {
      GList *node;
      GList *node_orig;
      Split *s, *so;

      s = so = NULL;

      for (i = 0, node = trans->splits, node_orig = orig->splits ;
           node && node_orig ;
           i++, node = node->next, node_orig = node_orig->next)
      {
         s = node->data;
         so = node_orig->data;

         if (so->acc != s->acc)
         {
           force_it = 1;
           mismatch = i;
           break;
         }

         g_cache_remove (gnc_engine_get_string_cache(), s->action);
         s->action = so->action;
         so->action = g_cache_insert(gnc_engine_get_string_cache(), "");

         g_cache_remove (gnc_engine_get_string_cache(), s->memo);
         s->memo = so->memo;
         so->memo = g_cache_insert(gnc_engine_get_string_cache(), "");

         kvp_frame_delete (s->kvp_data);
         s->kvp_data = so->kvp_data;
         if (!s->kvp_data)
           s->kvp_data = kvp_frame_new ();
         so->kvp_data = kvp_frame_new ();

         s->reconciled  = so->reconciled;
         s->amount      = so->amount;
         s->value       = so->value;

         s->date_reconciled = so->date_reconciled;

         /* do NOT check date order until all of the other fields 
          * have been properly restored */
         mark_split (s);
         xaccAccountFixSplitDateOrder (s->acc, s); 
         xaccAccountRecomputeBalance (s->acc);
         gen_event (s);
      }

      /* if the number of splits were not identical... then force */
      if (node || node_orig)
      {
        force_it = 1;
        mismatch = i;
      }
   }

   /* OK, if force_it got set, we'll have to tough it out and brute-force
    * the rest of the way.  Clobber all the edited splits, add all new splits.
    * Unfortunately, this can suck up CPU cycles in the Remove/Insert routines.
    */  
   if (force_it)
   {
      GList *node;

      /* In this loop, we tuck the fixed-up splits back into 
       * orig array, for temp safekeeping. */
      for (i = 0, node = trans->splits ;
           node && i < mismatch ;
           i++, node = node->next)
      {
         Split *s = node->data;
         GList *node_orig;

         node_orig = g_list_nth (orig->splits, i);
         xaccFreeSplit (node_orig->data);
         node_orig->data = s;
      }

      /* in this loop, we remove excess new splits that had been added */
      for (node = g_list_nth (trans->splits, mismatch) ;
           node ; node = node->next)
      {
         Split *s = node->data;
         Account *acc = s->acc;

         mark_split (s);
         xaccAccountRemoveSplit (acc, s);
         xaccAccountRecomputeBalance (acc);
         gen_event (s);
         xaccRemoveEntity(s->book->entity_table, &s->guid);
         xaccFreeSplit (s);
      }

      g_list_free (trans->splits);

      trans->splits = orig->splits;
      orig->splits = NULL;

      /* in this loop, we fix up the remaining orig splits to be healthy */
      for (node = g_list_nth (trans->splits, mismatch) ;
           node ; node = node->next)
      {
         Split *s = node->data;
         Account *account = s->acc;

         s->parent = trans;
         s->acc = NULL;
         xaccStoreEntity(s->book->entity_table, s, &s->guid, GNC_ID_SPLIT);
         xaccAccountInsertSplit (account, s);
         mark_split (s);
         xaccAccountRecomputeBalance (account);
         gen_event (s);
      }
   }

   /* Now that the engine copy is back to its original version,
    * get the backend to fix it in the database */
   be = xaccTransactionGetBackend (trans);
   if (be && be->rollback) 
   {
      GNCBackendError errcode;

      /* clear errors */
      do {
        errcode = xaccBackendGetError (be);
      } while (ERR_BACKEND_NO_ERR != errcode);

      (be->rollback) (be, GNC_ID_TRANS, trans);

      errcode = xaccBackendGetError (be);
      if (ERR_BACKEND_MOD_DESTROY == errcode)
      {
         /* The backend is asking us to delete this transaction.
          * This typically happens because another (remote) user
          * has deleted this transaction, and we haven't found
          * out about it until this user tried to edit it.
          */
         xaccTransDestroy (trans);
         xaccFreeTransaction (trans);

         /* push error back onto the stack */
         xaccBackendSetError (be, errcode);
         LEAVE ("deleted trans addr=%p\n", trans);
         return;
      }
      if (ERR_BACKEND_NO_ERR != errcode) 
      {
        PERR ("Rollback Failed.  Ouch!");
        /* push error back onto the stack */
        xaccBackendSetError (be, errcode);
      }
   }

   xaccTransWriteLog (trans, 'R');

   xaccFreeTransaction (trans->orig);

   trans->orig = NULL;
   trans->do_free = FALSE;

   /* Put back to zero. */
   trans->editlevel--;

   LEAVE ("trans addr=%p\n", trans);
}

gboolean
xaccTransIsOpen (const Transaction *trans)
{
  if (!trans) return FALSE;
  return (0 < trans->editlevel);
}

void
xaccTransSetVersion (Transaction *trans, gint32 vers)
{
  if (!trans) return;
  trans->version = vers;
}

gint32
xaccTransGetVersion (const Transaction *trans)
{
  if (!trans) return 0;
  return (trans->version);
}

/********************************************************************\
\********************************************************************/

gboolean
xaccTransWarnReadOnly (const Transaction *trans)
{
  const gchar *reason;

  if (!trans) return FALSE;

  reason = xaccTransGetReadOnly (trans);
  if (reason) {
    gnc_send_gui_error("Cannot modify or delete this transaction.\n"
		       "This transaction is marked read-only because:\n\n'%s'",
		       reason);
    return TRUE;
  }
  return FALSE;
}

/********************************************************************\
\********************************************************************/

void
xaccTransDestroy (Transaction *trans)
{
  GList *node;

  if (!trans) return;
  check_open (trans);

  if (xaccTransWarnReadOnly (trans))
    return;

  trans->do_free = TRUE;
  xaccTransWriteLog (trans, 'D');

  gnc_engine_generate_event (&trans->guid, GNC_EVENT_DESTROY);

  for (node = trans->splits; node; node = node->next)
  {
    Split *split = node->data;

    mark_split (split);
    xaccAccountRemoveSplit (split->acc, split);
    xaccAccountRecomputeBalance (split->acc);
    gen_event (split);
    xaccRemoveEntity(split->book->entity_table, &split->guid);
    xaccFreeSplit (split);

    node->data = NULL;
  }

  g_list_free (trans->splits);
  trans->splits = NULL;

  xaccRemoveEntity(trans->book->entity_table, &trans->guid);

  /* the actual free is done with the commit call, else its rolled back */
  /* xaccFreeTransaction (trans);  don't do this here ... */
}

/********************************************************************\
 * TransRemoveSplit is an engine private function and does not/should
 * not cause any rebalancing to occur.
\********************************************************************/

static void
xaccTransRemoveSplit (Transaction *trans, Split *split) 
{
  if (trans == NULL)
    return;

  trans->splits = g_list_remove (trans->splits, split);
}

/********************************************************************\
\********************************************************************/

gboolean
xaccSplitDestroy (Split *split)
{
   Account *acc;
   Transaction *trans;

   if (!split) return TRUE;

   acc = split->acc;
   trans = split->parent;
   if (acc && !acc->do_free && xaccTransWarnReadOnly (trans))
       return FALSE;

   check_open (trans);

   mark_split (split);

   if (trans)
   {
     gboolean ismember = (g_list_find (trans->splits, split) != NULL);

     if (!ismember)
     {
       PERR ("split not in transaction");
     }
     else
       xaccTransRemoveSplit (trans, split);
   }

   /* Note: split is removed from lot when its removed from accoount */
   xaccAccountRemoveSplit (acc, split);
   xaccAccountRecomputeBalance (acc);

   gen_event (split);
   xaccRemoveEntity (split->book->entity_table, &split->guid);
   xaccFreeSplit (split);
   return TRUE;
}

/********************************************************************\
\********************************************************************/

void
xaccTransAppendSplit (Transaction *trans, Split *split) 
{
   Transaction *oldtrans;

   if (!trans || !split) return;
   g_return_if_fail (trans->book == split->book);
   check_open (trans);

   /* first, make sure that the split isn't already inserted 
    * elsewhere. If so, then remove it. */
   oldtrans = split->parent;
   if (oldtrans)
      xaccTransRemoveSplit (oldtrans, split);

   /* now, insert the split into the array */
   split->parent = trans;
   trans->splits = g_list_append (trans->splits, split);

   /* convert the split to the new transaction's commodity denominator */
   /* if the denominator can't be exactly converted, it's an error */
   if (trans->common_currency)
   {
     int fraction = gnc_commodity_get_fraction (trans->common_currency);
     gnc_numeric new_value;

     new_value = gnc_numeric_convert(split->value, fraction, GNC_RND_ROUND);
     if (gnc_numeric_check (new_value) == GNC_ERROR_OK)
       split->value = new_value;
   }
}

/********************************************************************\
 * sorting comparison function
 *
 * returns a negative value if transaction a is dated earlier than b, 
 * returns a positive value if transaction a is dated later than b, 
 *
 * This function tries very hard to uniquely order all transactions.
 * If two transactions occur on the same date, then their "num" fields
 * are compared.  If the num fields are identical, then the description
 * fields are compared.  If these are identical, then the memo fields 
 * are compared.  Hopefully, there will not be any transactions that
 * occur on the same day that have all three of these values identical.
 *
 * Note that being able to establish this kind of absolute order is 
 * important for some of the ledger display functions.
 *
 * Yes, this kind of code dependency is ugly, but the alternatives seem
 * ugly too.
 *
\********************************************************************/


#define DATE_CMP(aaa,bbb,field) {			\
  /* if dates differ, return */				\
  if ( (aaa->field.tv_sec) <				\
       (bbb->field.tv_sec)) {				\
    return -1;						\
  } else						\
  if ( (aaa->field.tv_sec) >				\
       (bbb->field.tv_sec)) {				\
    return +1;						\
  }							\
							\
  /* else, seconds match. check nanoseconds */		\
  if ( (aaa->field.tv_nsec) <				\
       (bbb->field.tv_nsec)) {				\
    return -1;						\
  } else						\
  if ( (aaa->field.tv_nsec) >				\
       (bbb->field.tv_nsec)) {				\
    return +1;						\
  }							\
}



int
xaccSplitDateOrder (const Split *sa, const Split *sb)
{
  int retval;
  int comp;
  char *da, *db;

  if(sa == sb) return 0;
  /* nothing is always less than something */
  if(!sa && sb) return -1;
  if(sa && !sb) return +1;

  retval = xaccTransOrder (sa->parent, sb->parent);
  if (0 != retval) return retval;

  /* otherwise, sort on memo strings */
  da = sa->memo;
  db = sb->memo;
  SAFE_STRCMP (da, db);

  /* otherwise, sort on action strings */
  da = sa->action;
  db = sb->action;
  SAFE_STRCMP (da, db);

  /* the reconciled flag ... */
  if ((sa->reconciled) < (sb->reconciled)) return -1;
  if ((sa->reconciled) > (sb->reconciled)) return +1;

  /* compare amounts */
  comp = gnc_numeric_compare(sa->amount, sb->amount);
  if(comp < 0) return -1;
  if(comp > 0) return +1;

  comp = gnc_numeric_compare(sa->value, sb->value);
  if(comp < 0) return -1;
  if(comp > 0) return +1;

  /* if dates differ, return */
  DATE_CMP(sa,sb,date_reconciled);

#if 0
  /* sort on txn guid. */
  if(sa->parent && !sb->parent) return -1;
  if(!sa->parent && sb->parent) return 1;
  if(sa->parent && sb->parent) {
    retval = guid_compare(&(sa->guid), &(sb->guid));
    if(retval != 0) return retval;
  }
#endif

  /* else, sort on guid - keeps sort stable. */
  retval = guid_compare(&(sa->guid), &(sb->guid));
  if(retval != 0) return retval;

  return 0;
}

int
xaccTransOrder (const Transaction *ta, const Transaction *tb)
{
  char *da, *db;
  int retval, na, nb;

  if ( ta && !tb ) return -1;
  if ( !ta && tb ) return +1;
  if ( !ta && !tb ) return 0;

  /* if dates differ, return */
  DATE_CMP(ta,tb,date_posted);

  /* otherwise, sort on number string */
  na = atoi(ta->num);
  nb = atoi(tb->num);
  if (na < nb) return -1;
  if (na > nb) return +1;

  /* if dates differ, return */
  DATE_CMP(ta,tb,date_entered);

  /* otherwise, sort on description string */
  da = ta->description;
  db = tb->description;
  SAFE_STRCMP (da, db);

  /* else, sort on guid - keeps sort stable. */
  retval = guid_compare(&(ta->guid), &(tb->guid));
  if(retval != 0) return retval;

  return 0;
}
static gboolean
get_corr_account_split(const Split *sa, Split **retval)
{
 
  Split *current_split;
  GList *split_list;
  Transaction * ta;
  gnc_numeric sa_value, current_value;
  gboolean sa_value_positive, current_value_positive, seen_different = FALSE;

  *retval = NULL;
  g_return_val_if_fail(sa, TRUE);
  ta = sa->parent;
  
  sa_value = sa->value;
  sa_value_positive = gnc_numeric_positive_p(sa_value);

  for (split_list = ta->splits;
       split_list; split_list = split_list->next)
  {
    current_split = split_list->data;
    if(current_split != sa)
    {
      current_value = current_split->value;
      current_value_positive = gnc_numeric_positive_p(current_value);
      if((sa_value_positive && !current_value_positive) || 
         (!sa_value_positive && current_value_positive))
      {
        if(seen_different)
        {
          *retval = NULL;
          return TRUE;
        }
        else
        {
          seen_different = TRUE;
          *retval = current_split;
        }
      }
    }
  }
  return FALSE;
}

const char *
xaccSplitGetCorrAccountName(const Split *sa)
{
  static const char *split_const = NULL;
  Split *other_split;
  Account *other_split_acc;

  if(get_corr_account_split(sa, &other_split))
  {
    if (!split_const)
      split_const = _("-- Split Transaction --");

    return split_const;
  }
  else
  {
    other_split_acc = xaccSplitGetAccount(other_split);
    return xaccAccountGetName(other_split_acc);
  }
}

char *
xaccSplitGetCorrAccountFullName(const Split *sa, char separator)
{
  static const char *split_const = NULL;
  Split *other_split;
  Account *other_split_acc;

  if(get_corr_account_split(sa, &other_split))
  {
    if (!split_const)
      split_const = _("-- Split Transaction --");

    return g_strdup(split_const);
  }
  else
  {
    other_split_acc = xaccSplitGetAccount(other_split);
    return xaccAccountGetFullName(other_split_acc, separator);
  }
}

const char *
xaccSplitGetCorrAccountCode(const Split *sa)
{
  static const char *split_const = NULL;
  Split *other_split;
  Account *other_split_acc;

  if(get_corr_account_split(sa, &other_split))
  {
    if (!split_const)
      split_const = _("Split");

    return split_const;
  }
  else
  {
    other_split_acc = xaccSplitGetAccount(other_split);
    return xaccAccountGetCode(other_split_acc);
  }
}

int 
xaccSplitCompareAccountFullNames(Split *sa, Split *sb)
{
  Account *aa, *ab;
  char *full_a, *full_b;
  int retval;
  if (!sa && !sb) return 0;
  if (!sa) return -1;
  if (!sb) return 1;

  aa = sa->acc;
  ab = sb->acc;
  full_a = xaccAccountGetFullName(aa, ':');
  full_b = xaccAccountGetFullName(ab, ':');
  /* for comparison purposes it doesn't matter what we use as a separator */
  retval = safe_strcmp(full_a, full_b);
  g_free(full_a);
  g_free(full_b);
  return retval;

}


int 
xaccSplitCompareAccountCodes(Split *sa, Split *sb)
{
  Account *aa, *ab;
  if (!sa && !sb) return 0;
  if (!sa) return -1;
  if (!sb) return 1;

  aa = sa->acc;
  ab = sb->acc;
  
  return safe_strcmp(xaccAccountGetName(aa), xaccAccountGetName(ab));
}

int 
xaccSplitCompareOtherAccountFullNames(Split *sa, Split *sb)
{
  char *ca, *cb; 
  int retval;
  if (!sa && !sb) return 0;
  if (!sa) return -1;
  if (!sb) return 1;

  /* doesn't matter what separator we use
   * as long as they are the same 
   */

  ca = xaccSplitGetCorrAccountFullName(sa, ':');
  cb = xaccSplitGetCorrAccountFullName(sb, ':');
  retval = safe_strcmp(ca, cb);
  g_free(ca);
  g_free(cb);
  return retval;
}

int
xaccSplitCompareOtherAccountCodes(Split *sa, Split *sb)
{
  const char *ca, *cb;
  if (!sa && !sb) return 0;
  if (!sa) return -1;
  if (!sb) return 1;

  ca = xaccSplitGetCorrAccountCode(sa);
  cb = xaccSplitGetCorrAccountCode(sb);
  return safe_strcmp(ca, cb);
}
/********************************************************************\
\********************************************************************/

enum { TDATE_POSTED, TDATE_ENTERED };

static void
xaccTransSetDateInternal(Transaction *trans, int which, time_t secs,
                         long int nsecs)
{
    Timespec *dadate = 0;
    if(!trans) return;
    check_open(trans);

    PINFO ("addr=%p set %d date to %lu %li %s",
	   trans, which, (long unsigned int)secs, nsecs, ctime (&secs));
    
    dadate = ((which == TDATE_POSTED)
              ? &trans->date_posted
              : &trans->date_entered);
    dadate->tv_sec = secs;
    dadate->tv_nsec = nsecs;

    mark_trans(trans);
    gen_event_trans (trans);
   /* Because the date has changed, we need to make sure that each of
    * the splits is properly ordered in each of their accounts. We
    * could do that here, simply by reinserting each split into its
    * account. However, in some ways this is bad behaviour, and it
    * seems much better/nicer to defer that until the commit phase,
    * i.e. until the user has called the xaccTransCommitEdit()
    * routine. So, for now, we are done. */
}

void
xaccTransSetDateSecs (Transaction *trans, time_t secs)
{
    xaccTransSetDateInternal(trans, TDATE_POSTED, secs, 0);
}

void
xaccTransSetDatePostedSecs (Transaction *trans, time_t secs)
{
    xaccTransSetDateInternal(trans, TDATE_POSTED, secs, 0);
}

void
xaccTransSetDateEnteredSecs (Transaction *trans, time_t secs)
{
    xaccTransSetDateInternal(trans, TDATE_ENTERED, secs, 0);
}

void
xaccTransSetDatePostedTS (Transaction *trans, const Timespec *ts)
{
   if (!ts) return;
   xaccTransSetDateInternal(trans, TDATE_POSTED, ts->tv_sec, ts->tv_nsec);
}

void
xaccTransSetDateEnteredTS (Transaction *trans, const Timespec *ts)
{
   if (!ts) return;
   xaccTransSetDateInternal(trans, TDATE_ENTERED, ts->tv_sec, ts->tv_nsec);
}

void
xaccTransSetDate (Transaction *trans, int day, int mon, int year) 
{
  Timespec ts = gnc_dmy2timespec(day, mon, year);
  xaccTransSetDateInternal(trans, TDATE_POSTED, ts.tv_sec, ts.tv_nsec);
}

void
xaccTransSetDateDueTS (Transaction *trans, const Timespec *ts)
{
  kvp_value *value;

  if (!trans || !ts) return;

  value = kvp_value_new_timespec (*ts);
  kvp_frame_set_slot_path (trans->kvp_data, value, TRANS_DATE_DUE_KVP, NULL);
  kvp_value_delete (value);
}

void
xaccTransSetTxnType (Transaction *trans, char type)
{
  kvp_value *value;
  char s[2];

  if (!trans) return;

  s[0] = type;
  s[1] = '\0';

  value = kvp_value_new_string (s);
  kvp_frame_set_slot_path (trans->kvp_data, value, TRANS_TXN_TYPE_KVP, NULL);
  kvp_value_delete (value);
}

void xaccTransClearReadOnly (Transaction *trans)
{
  if (!trans) return;

  kvp_frame_set_slot_path (trans->kvp_data, NULL, TRANS_READ_ONLY_REASON, NULL);
}

void
xaccTransSetReadOnly (Transaction *trans, const char *reason)
{
  if (!trans || !reason) return;

  kvp_frame_set_slot_nc (trans->kvp_data, TRANS_READ_ONLY_REASON, 
			 kvp_value_new_string (reason));
}

/********************************************************************\
\********************************************************************/

void
xaccTransSetNum (Transaction *trans, const char *xnum)
{
   char * tmp;
   if (!trans || !xnum) return;
   check_open (trans);

   tmp = g_cache_insert(gnc_engine_get_string_cache(), (gpointer) xnum);
   g_cache_remove(gnc_engine_get_string_cache(), trans->num);
   trans->num = tmp;
   gen_event_trans (trans);
}

void
xaccTransSetDescription (Transaction *trans, const char *desc)
{
   char * tmp;
   if (!trans || !desc) return;
   check_open (trans);

   tmp = g_cache_insert(gnc_engine_get_string_cache(), (gpointer) desc);
   g_cache_remove(gnc_engine_get_string_cache(), trans->description);
   trans->description = tmp;
   gen_event_trans (trans);
}

void
xaccTransSetNotes (Transaction *trans, const char *notes)
{
  if (!trans || !notes) return;
  check_open (trans);

  kvp_frame_set_slot_nc (trans->kvp_data, trans_notes_str, 
                                    kvp_value_new_string (notes));
  gen_event_trans (trans);
}

/********************************************************************\
\********************************************************************/

Split *
xaccTransGetSplit (const Transaction *trans, int i) 
{
   if (!trans) return NULL;
   if (i < 0) return NULL;

   return g_list_nth_data (trans->splits, i);
}

SplitList *
xaccTransGetSplitList (const Transaction *trans)
{
  if (!trans) return NULL;

  return trans->splits;
}

const char *
xaccTransGetNum (const Transaction *trans)
{
   if (!trans) return NULL;
   return (trans->num);
}

const char * 
xaccTransGetDescription (const Transaction *trans)
{
   if (!trans) return NULL;
   return (trans->description);
}

const char * 
xaccTransGetNotes (const Transaction *trans)
{
  kvp_value *v;

  if (!trans) return NULL;

  v = kvp_frame_get_slot (trans->kvp_data, trans_notes_str);
  if (!v)
    return NULL;

  return kvp_value_get_string (v);
}

time_t
xaccTransGetDate (const Transaction *trans)
{
   if (!trans) return 0;
   return (trans->date_posted.tv_sec);
}

void
xaccTransGetDatePostedTS (const Transaction *trans, Timespec *ts)
{
   if (!trans || !ts) return;
   *ts = (trans->date_posted);
}

void
xaccTransGetDateEnteredTS (const Transaction *trans, Timespec *ts)
{
   if (!trans || !ts) return;
   *ts = (trans->date_entered);
}

Timespec
xaccTransRetDatePostedTS (const Transaction *trans)
{
   Timespec ts;
   ts.tv_sec = 0; ts.tv_nsec = 0;
   if (!trans) return ts;
   return (trans->date_posted);
}

Timespec
xaccTransRetDateEnteredTS (const Transaction *trans)
{
   Timespec ts;
   ts.tv_sec = 0; ts.tv_nsec = 0;
   if (!trans) return ts;
   return (trans->date_entered);
}

void
xaccTransGetDateDueTS (const Transaction *trans, Timespec *ts)
{
  kvp_value *value;

  if (!trans || !ts) return;

  value = kvp_frame_get_slot_path (trans->kvp_data, TRANS_DATE_DUE_KVP, NULL);
  if (value)
    *ts = kvp_value_get_timespec (value);
  else
    xaccTransGetDatePostedTS (trans, ts);
}

Timespec
xaccTransRetDateDueTS (const Transaction *trans)
{
  Timespec ts;
  ts.tv_sec = 0; ts.tv_nsec = 0;
  if (!trans) return ts;
  xaccTransGetDateDueTS (trans, &ts);
  return ts;
}

char
xaccTransGetTxnType (const Transaction *trans)
{
  kvp_value *value;
  const char *s;

  if (!trans) return TXN_TYPE_NONE;

  value = kvp_frame_get_slot_path (trans->kvp_data, TRANS_TXN_TYPE_KVP, NULL);
  if (value) {
    s = kvp_value_get_string (value);
    return *s;
  }
  return TXN_TYPE_NONE;
}

const char * 
xaccTransGetReadOnly (const Transaction *trans)
{
  kvp_value *v;

  if (!trans) return NULL;

  v = kvp_frame_get_slot (trans->kvp_data, TRANS_READ_ONLY_REASON);
  if (!v)
    return NULL;

  return kvp_value_get_string (v);
}

int
xaccTransCountSplits (const Transaction *trans)
{
   if (!trans) return 0;
   return g_list_length (trans->splits);
}

gboolean
xaccTransHasReconciledSplitsByAccount (const Transaction *trans, 
				       const Account *account)
{
  GList *node;

  for (node = xaccTransGetSplitList (trans); node; node = node->next)
  {
    Split *split = node->data;

    if (account && (xaccSplitGetAccount(split) != account))
      continue;

    switch (xaccSplitGetReconcile (split))
    {
      case YREC:
      case FREC:
        return TRUE;

      default:
        break;
    }
  }

  return FALSE;
}

gboolean
xaccTransHasReconciledSplits (const Transaction *trans)
{
  return xaccTransHasReconciledSplitsByAccount (trans, NULL);
}


gboolean
xaccTransHasSplitsInStateByAccount (const Transaction *trans,
				    const char state,
				    const Account *account)
{
  GList *node;

  for (node = xaccTransGetSplitList (trans); node; node = node->next)
  {
    Split *split = node->data;

    if (account && (split->acc != account))
      continue;

    if (split->reconciled == state)
      return TRUE;
  }

  return FALSE;
}

gboolean
xaccTransHasSplitsInState (const Transaction *trans, const char state)
{
  return xaccTransHasSplitsInStateByAccount (trans, state, NULL);
}


/********************************************************************\
\********************************************************************/

void
xaccSplitSetMemo (Split *split, const char *memo)
{
   char * tmp;
   if (!split || !memo) return;
   check_open (split->parent);

   tmp = g_cache_insert(gnc_engine_get_string_cache(), (gpointer) memo);
   g_cache_remove(gnc_engine_get_string_cache(), split->memo);
   split->memo = tmp;
   gen_event (split);
}

void
xaccSplitSetAction (Split *split, const char *actn)
{
   char * tmp;
   if (!split || !actn) return;
   check_open (split->parent);

   tmp = g_cache_insert(gnc_engine_get_string_cache(), (gpointer) actn);
   g_cache_remove(gnc_engine_get_string_cache(), split->action);
   split->action = tmp;
   gen_event (split);
}

void
xaccSplitSetReconcile (Split *split, char recn)
{
   if (!split) return;
   check_open (split->parent);

   switch (recn)
   {
   case NREC:
   case CREC:
   case YREC:
   case FREC:
   case VREC:
     break;
   default:
     PERR("Bad reconciled flag");
     return;
   }

   if (split->reconciled != recn)
   {
     Account *account = split->acc;

     split->reconciled = recn;
     mark_split (split);
     xaccAccountRecomputeBalance (account);
     gen_event (split);
   }
}

void
xaccSplitSetDateReconciledSecs (Split *split, time_t secs)
{
   if (!split) return;
   check_open (split->parent);

   split->date_reconciled.tv_sec = secs;
   split->date_reconciled.tv_nsec = 0;
   gen_event (split);
}

void
xaccSplitSetDateReconciledTS (Split *split, Timespec *ts)
{
   if (!split || !ts) return;
   check_open (split->parent);

   split->date_reconciled = *ts;
   gen_event (split);
}

void
xaccSplitGetDateReconciledTS (const Split * split, Timespec *ts)
{
   if (!split || !ts) return;
   *ts = (split->date_reconciled);
}

Timespec
xaccSplitRetDateReconciledTS (const Split * split)
{
   Timespec ts; ts.tv_sec=0; ts.tv_nsec=0;
   if (!split) return ts;
   return (split->date_reconciled);
}

/********************************************************************\
\********************************************************************/

/* return the parent transaction of the split */
Transaction * 
xaccSplitGetParent (const Split *split)
{
   if (!split) return NULL;
   return (split->parent);
}

GNCLot *
xaccSplitGetLot (const Split *split)
{
   if (!split) return NULL;
   return (split->lot);
}

const char *
xaccSplitGetMemo (const Split *split)
{
   if (!split) return NULL;
   return (split->memo);
}

const char *
xaccSplitGetAction (const Split *split)
{
   if (!split) return NULL;
   return (split->action);
}

char 
xaccSplitGetReconcile (const Split *split) 
{
  if (!split) return ' ';
  return (split->reconciled);
}

double
DxaccSplitGetShareAmount (const Split * split) 
{
  if (!split) return 0.0;
  return gnc_numeric_to_double(split->amount);
}

double
DxaccSplitGetValue (const Split * split) 
{
  if (!split) return 0.0;
  return gnc_numeric_to_double(split->value);
}

double
DxaccSplitGetSharePrice (const Split * split)
{
  return gnc_numeric_to_double(xaccSplitGetSharePrice(split));
}

gnc_numeric
xaccSplitGetAmount (const Split * split)
{
  if (!split) return gnc_numeric_zero();
  return split->amount;
}

gnc_numeric
xaccSplitGetValue (const Split * split) 
{
  if (!split) return gnc_numeric_zero();
  return split->value; 
}

gnc_numeric
xaccSplitGetSharePrice (const Split * split) 
{
  if(!split || gnc_numeric_zero_p(split->amount)) 
  {
    return gnc_numeric_create(1, 1);
  }
  return gnc_numeric_div(split->value, 
                         split->amount,
                         GNC_DENOM_AUTO, 
                         GNC_DENOM_SIGFIGS(PRICE_SIGFIGS) |
                         GNC_RND_ROUND);
}

/********************************************************************\
\********************************************************************/

QofBook *
xaccSplitGetBook (const Split *split)
{
  if (!split) return NULL;
  return split->book;
}

const char *
xaccSplitGetType(const Split *s)
{
  kvp_frame *frame;
  kvp_value *split_type;

  if(!s) return NULL;
  frame = s->kvp_data;
  if(!frame) return NULL;
  split_type = kvp_frame_get_slot(frame, "split-type");
  if(!split_type) return "normal";
  if(kvp_value_get_type(split_type) != KVP_TYPE_STRING) return NULL;
  return(kvp_value_get_string(split_type));
}

/* reconfigure a split to be a stock split - after this, you shouldn't
   mess with the value, just the amount. */
void
xaccSplitMakeStockSplit(Split *s)
{
  check_open (s->parent);

  s->value = gnc_numeric_zero();
  kvp_frame_set_slot_nc(s->kvp_data,
                        "split-type",
                        kvp_value_new_string("stock-split"));
  mark_split(s);
  gen_event (s);
}


/* ====================================================================== */

static gboolean
counter_thunk(Transaction *t, void *data)
{
    (*((guint*)data))++;
    return TRUE;
}

guint
gnc_book_count_transactions(QofBook *book)
{
    guint count = 0;
    xaccGroupForEachTransaction(xaccGetAccountGroup(book),
                                counter_thunk, (void*)&count);
    return count;
}

/********************************************************************\
\********************************************************************/

Account *
xaccGetAccountByName (Transaction *trans, const char * name)
{
   Account *acc = NULL;
   GList *node;

   if (!trans) return NULL;
   if (!name) return NULL;

   /* walk through the splits, looking for one, any one, that has a
    * parent account */
   for (node = trans->splits; node; node = node->next)
   {
     Split *s = node->data;

     acc = s->acc;
     if (acc) break;
   }
   
   if (!acc) return NULL;

   return xaccGetPeerAccountFromName (acc, name);
}

/********************************************************************\
\********************************************************************/

Account *
xaccGetAccountByFullName (Transaction *trans, const char * name,
                          const char separator)
{
   Account *acc = NULL;
   GList *node;

   if (!trans) return NULL;
   if (!name) return NULL;

   /* walk through the splits, looking for one, any one, that has a
    * parent account */
   for (node = trans->splits; node; node = node->next)
   {
     Split *s = node->data;

     acc = s->acc;
     if (acc) break;
   }
   
   if (!acc) return NULL;

   return xaccGetPeerAccountFromFullName (acc, name, separator);
}

/********************************************************************\
\********************************************************************/

Split *
xaccSplitGetOtherSplit (const Split *split)
{
  Split *s1, *s2;
  Transaction *trans;

  if (!split) return NULL;
  trans = split->parent;

  if (g_list_length (trans->splits) != 2)
    return NULL;

  s1 = g_list_nth_data (trans->splits, 0);
  s2 = g_list_nth_data (trans->splits, 1);

  if (s1 == split)
    return s2;

  return s1;
}

/********************************************************************\
\********************************************************************/

gboolean
xaccIsPeerSplit (const Split *sa, const Split *sb)
{
   Transaction *ta, *tb;
   if (!sa || !sb) return 0;
   ta = sa->parent;
   tb = sb->parent;
   if (ta == tb) return 1;
   return 0;
}


/********************************************************************\
\********************************************************************/

void
xaccTransVoid(Transaction *transaction,
              const char *reason)
{
  kvp_frame *frame;
  kvp_value *val;
  gnc_numeric amt, zero;
  GList *split_list;
  Split *split;
  Timespec now;
  char iso8601_str[ISO_DATELENGTH+1] = "";

  g_return_if_fail(transaction && reason);

  xaccTransBeginEdit(transaction);
  zero = gnc_numeric_zero();
  frame = transaction->kvp_data;

  val = kvp_frame_get_slot(frame, trans_notes_str);
  kvp_frame_set_slot(frame, void_former_notes_str, val);
  val = kvp_value_new_string("Voided transaction");
  kvp_frame_set_slot_nc(frame, trans_notes_str, val);

  val = kvp_value_new_string(reason);
  kvp_frame_set_slot_nc(frame, void_reason_str, val);

  now.tv_sec = time(NULL);
  now.tv_nsec = 0;
  gnc_timespec_to_iso8601_buff(now, iso8601_str);
  val = kvp_value_new_string (iso8601_str);
  kvp_frame_set_slot_nc(frame, void_time_str, val);

  for (split_list = transaction->splits; 
           split_list; 
           split_list = g_list_next(split_list))
  {
    split = split_list->data;
    frame = split->kvp_data;

    amt = split->amount;
    val = kvp_value_new_gnc_numeric(amt);
    kvp_frame_set_slot_nc(frame, void_former_amt_str, val);
    
    amt = split->value;
    val = kvp_value_new_gnc_numeric(amt);
    kvp_frame_set_slot_nc(frame, void_former_val_str, val);

    xaccSplitSetAmount (split, zero);
    xaccSplitSetValue (split, zero);
    xaccSplitSetReconcile(split, VREC);
  }

  xaccTransSetReadOnly(transaction, "Transaction Voided");
  xaccTransCommitEdit(transaction);
}

gboolean 
xaccTransGetVoidStatus(const Transaction *trans)
{
  g_return_val_if_fail(trans, FALSE);

  return (kvp_frame_get_slot(trans->kvp_data, void_reason_str) != NULL);
}

char *
xaccTransGetVoidReason(const Transaction *trans)
{
  kvp_value *val;
  char *reason;
  g_return_val_if_fail(trans, NULL);

  val = kvp_frame_get_slot(trans->kvp_data, void_reason_str);
  
  if(val)
  {
    reason = kvp_value_get_string(val);
    return reason;
  }

  return NULL;
}

gnc_numeric
xaccSplitVoidFormerAmount(const Split *split)
{
  kvp_value *val;
  gnc_numeric amt = gnc_numeric_zero();
  g_return_val_if_fail(split, amt);

  val = kvp_frame_get_slot(split->kvp_data, void_former_amt_str);
  
  if(val)
  {
    amt = kvp_value_get_numeric(val);
  }

  return amt;
}

gnc_numeric
xaccSplitVoidFormerValue(const Split *split)
{
  kvp_value *val;
  gnc_numeric amt = gnc_numeric_zero();

  g_return_val_if_fail(split, amt);

  val = kvp_frame_get_slot(split->kvp_data, void_former_val_str);
  
  if(val)
  {
    amt = kvp_value_get_numeric(val);
  }

  return amt;
}

Timespec
xaccTransGetVoidTime(const Transaction *tr)
{
  kvp_value *val;
  Timespec void_time = {0,0};

  g_return_val_if_fail(tr, void_time);

  val = kvp_frame_get_slot(tr->kvp_data, void_time_str);
  
  if(val)
  {
    void_time = gnc_iso8601_to_timespec_local(kvp_value_get_string(val));
  }

  return void_time;
}

void
xaccTransUnvoid (Transaction *transaction)
{
  kvp_frame *frame;
  kvp_value *val;
  gnc_numeric amt;
  GList *split_list;
  Split *split;

  g_return_if_fail(transaction);

  frame = transaction->kvp_data;
  val = kvp_frame_get_slot(frame, void_reason_str);
  if (val == NULL){
    /* Transaction isn't voided. Bail. */
    return;
  }

  xaccTransBeginEdit(transaction);

  val = kvp_frame_get_slot(frame, void_former_notes_str);
  kvp_frame_set_slot(frame, trans_notes_str, val);
  kvp_frame_set_slot_nc(frame, void_former_notes_str, NULL);
  kvp_frame_set_slot_nc(frame, void_reason_str, NULL);
  kvp_frame_set_slot_nc(frame, void_time_str, NULL);

  for (split_list = transaction->splits; 
           split_list; 
           split_list = g_list_next(split_list))
  {
    split = split_list->data;
    frame = split->kvp_data;
    
    val = kvp_frame_get_slot(frame, void_former_amt_str);
    amt = kvp_value_get_numeric(val);
    xaccSplitSetAmount (split, amt);
    kvp_frame_set_slot(frame, void_former_amt_str, NULL);
    
    val = kvp_frame_get_slot(frame, void_former_val_str);
    amt = kvp_value_get_numeric(val);
    xaccSplitSetValue (split, amt);
    kvp_frame_set_slot(frame, void_former_val_str, NULL);

    xaccSplitSetReconcile(split, NREC);
  }

  xaccTransClearReadOnly(transaction);
  xaccTransCommitEdit(transaction);
}

void
xaccTransReverse (Transaction *trans)
{
  GList *split_list;
  Split *split;

  g_return_if_fail(trans);

  gnc_engine_suspend_events();
  xaccTransBeginEdit(trans);

  /* Reverse the values on each split. Clear per-split info. */
  for (split_list = trans->splits; 
           split_list; 
           split_list = g_list_next(split_list))
  {
    split = split_list->data;
    split->amount = gnc_numeric_neg(split->amount);
    split->value = gnc_numeric_neg(split->value);
    split->reconciled = NREC;
    xaccSplitSetDateReconciledSecs (split, 0);
  }

  xaccTransCommitEdit(trans);

  gnc_engine_resume_events();
}

/********************************************************************\
\********************************************************************/

Backend *
xaccTransactionGetBackend (Transaction *trans)
{
  if (!trans || !trans->book) return NULL;
  return trans->book->backend;
}

/********************************************************************\
\********************************************************************/
/* gncObject function implementation */
static void
do_foreach (QofBook *book, GNCIdType type, foreachObjectCB cb, gpointer ud)
{
  GNCEntityTable *et;

  g_return_if_fail (book);
  g_return_if_fail (cb);

  et = qof_book_get_entity_table (book);
  xaccForeachEntity (et, type, cb, ud);
}

static void
split_foreach (QofBook *book, foreachObjectCB fcn, gpointer user_data)
{
  do_foreach (book, GNC_ID_SPLIT, fcn, user_data);
}

/* hook into the gncObject registry */

static QofObject split_object_def = {
  GNC_OBJECT_VERSION,
  GNC_ID_SPLIT,
  "Split",
  NULL,				/* book_begin */
  NULL,				/* book_end */
  NULL,				/* is_dirty */
  NULL,				/* mark_clean */
  split_foreach,		/* foreach */
  (const char* (*)(gpointer)) xaccSplitGetMemo		/* printable */
};

static gpointer split_account_guid_getter (gpointer obj)
{
  Split *s = obj;
  Account *acc;

  if (!s) return NULL;
  acc = xaccSplitGetAccount (s);
  if (!acc) return NULL;
  return ((gpointer)xaccAccountGetGUID (acc));
}

static gpointer no_op (gpointer obj)
{
  return obj;
}

gboolean xaccSplitRegister (void)
{
  static const QueryObjectDef params[] = {
    { SPLIT_KVP, QUERYCORE_KVP, (QueryAccess)xaccSplitGetSlots },
    { SPLIT_DATE_RECONCILED, QUERYCORE_DATE,
      (QueryAccess)xaccSplitRetDateReconciledTS },
    { "d-share-amount", QUERYCORE_DOUBLE,
      (QueryAccess)DxaccSplitGetShareAmount },
    { "d-share-int64", QUERYCORE_INT64, (QueryAccess)xaccSplitGetGUID },
    { SPLIT_BALANCE, QUERYCORE_NUMERIC, (QueryAccess)xaccSplitGetBalance },
    { SPLIT_CLEARED_BALANCE, QUERYCORE_NUMERIC,
      (QueryAccess)xaccSplitGetClearedBalance },
    { SPLIT_RECONCILED_BALANCE, QUERYCORE_NUMERIC,
      (QueryAccess)xaccSplitGetReconciledBalance },
    { SPLIT_MEMO, QUERYCORE_STRING, (QueryAccess)xaccSplitGetMemo },
    { SPLIT_ACTION, QUERYCORE_STRING, (QueryAccess)xaccSplitGetAction },
    { SPLIT_RECONCILE, QUERYCORE_CHAR, (QueryAccess)xaccSplitGetReconcile },
    { SPLIT_AMOUNT, QUERYCORE_NUMERIC, (QueryAccess)xaccSplitGetAmount },
    { SPLIT_SHARE_PRICE, QUERYCORE_NUMERIC,
      (QueryAccess)xaccSplitGetSharePrice },
    { SPLIT_VALUE, QUERYCORE_DEBCRED, (QueryAccess)xaccSplitGetValue },
    { SPLIT_TYPE, QUERYCORE_STRING, (QueryAccess)xaccSplitGetType },
    { SPLIT_VOIDED_AMOUNT, QUERYCORE_NUMERIC,
      (QueryAccess)xaccSplitVoidFormerAmount },
    { SPLIT_VOIDED_VALUE, QUERYCORE_NUMERIC,
      (QueryAccess)xaccSplitVoidFormerValue },
    { SPLIT_LOT, GNC_ID_LOT, (QueryAccess)xaccSplitGetLot },
    { SPLIT_TRANS, GNC_ID_TRANS, (QueryAccess)xaccSplitGetParent },
    { SPLIT_ACCOUNT, GNC_ID_ACCOUNT, (QueryAccess)xaccSplitGetAccount },
    { SPLIT_ACCOUNT_GUID, QUERYCORE_GUID, split_account_guid_getter },
    { SPLIT_ACCT_FULLNAME, SPLIT_ACCT_FULLNAME, no_op },
    { SPLIT_CORR_ACCT_NAME, SPLIT_CORR_ACCT_NAME, no_op },
    { SPLIT_CORR_ACCT_CODE, SPLIT_CORR_ACCT_CODE, no_op },
    { QUERY_PARAM_BOOK, GNC_ID_BOOK, (QueryAccess)xaccSplitGetBook },
    { QUERY_PARAM_GUID, QUERYCORE_GUID, (QueryAccess) xaccSplitGetGUID },
    { NULL },
  };

  gncQueryObjectRegister (GNC_ID_SPLIT, (QuerySort)xaccSplitDateOrder, params);
  gncQueryObjectRegister (SPLIT_ACCT_FULLNAME,
			  (QuerySort)xaccSplitCompareAccountFullNames,
			  NULL);
  gncQueryObjectRegister (SPLIT_CORR_ACCT_NAME,
			  (QuerySort)xaccSplitCompareOtherAccountFullNames,
			  NULL);
  gncQueryObjectRegister (SPLIT_CORR_ACCT_CODE,
			  (QuerySort)xaccSplitCompareOtherAccountCodes,
			  NULL);

  return qof_object_register (&split_object_def);
}

static void
trans_foreach (QofBook *book, foreachObjectCB fcn, gpointer user_data)
{
  do_foreach (book, GNC_ID_TRANS, fcn, user_data);
}

static QofObject trans_object_def = {
  GNC_OBJECT_VERSION,
  GNC_ID_TRANS,
  "Transaction",
  NULL,				/* book_begin */
  NULL,				/* book_end */
  NULL,				/* is_dirty */
  NULL,				/* mark_clean */
  trans_foreach,		/* foreach */
  (const char* (*)(gpointer)) xaccTransGetDescription	/* printable */
};

static gboolean
trans_is_balanced_p (const Transaction *txn)
{
  if (!txn)
    return FALSE;
  return (gnc_numeric_zero_p (xaccTransGetImbalance (txn)));
}

gboolean xaccTransRegister (void)
{
  static QueryObjectDef params[] = {
    { TRANS_KVP, QUERYCORE_KVP, (QueryAccess)xaccTransGetSlots },
    { TRANS_NUM, QUERYCORE_STRING, (QueryAccess)xaccTransGetNum },
    { TRANS_DESCRIPTION, QUERYCORE_STRING, (QueryAccess)xaccTransGetDescription },
    { TRANS_DATE_ENTERED, QUERYCORE_DATE, (QueryAccess)xaccTransRetDateEnteredTS },
    { TRANS_DATE_POSTED, QUERYCORE_DATE, (QueryAccess)xaccTransRetDatePostedTS },
    { TRANS_DATE_DUE, QUERYCORE_DATE, (QueryAccess)xaccTransRetDateDueTS },
    { TRANS_IMBALANCE, QUERYCORE_NUMERIC, (QueryAccess)xaccTransGetImbalance },
    { TRANS_NOTES, QUERYCORE_STRING, (QueryAccess)xaccTransGetNotes },
    { TRANS_IS_BALANCED, QUERYCORE_BOOLEAN, (QueryAccess)trans_is_balanced_p },
    { TRANS_TYPE, QUERYCORE_CHAR, (QueryAccess)xaccTransGetTxnType },
    { TRANS_VOID_STATUS, QUERYCORE_BOOLEAN, (QueryAccess)xaccTransGetVoidStatus },
    { TRANS_VOID_REASON, QUERYCORE_STRING, (QueryAccess)xaccTransGetVoidReason },
    { TRANS_VOID_TIME, QUERYCORE_DATE, (QueryAccess)xaccTransGetVoidTime },
    { TRANS_SPLITLIST, GNC_ID_SPLIT, (QueryAccess)xaccTransGetSplitList },
    { QUERY_PARAM_BOOK, GNC_ID_BOOK, (QueryAccess)xaccTransGetBook },
    { QUERY_PARAM_GUID, QUERYCORE_GUID, (QueryAccess)xaccTransGetGUID },
    { NULL },
  };

  gncQueryObjectRegister (GNC_ID_TRANS, (QuerySort)xaccTransOrder, params);

  return qof_object_register (&trans_object_def);
}

/************************ END OF ************************************\
\************************* FILE *************************************/