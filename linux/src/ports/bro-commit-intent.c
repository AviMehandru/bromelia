/* bro-commit-intent.c */
#include "bro-commit-intent.h"

BroCommitIntent *
bro_commit_intent_new (void)
{
  BroCommitIntent *x = g_new0 (BroCommitIntent, 1);
  x->items = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_commit_item_free);
  return x;
}

void
bro_commit_intent_free (BroCommitIntent *x)
{
  if (!x)
    return;
  g_free (x->staging);
  g_free (x->destination);
  g_clear_pointer (&x->items, g_ptr_array_unref);
  g_free (x);
}

BroCommitIntent *
bro_commit_intent_copy (const BroCommitIntent *x)
{
  BroCommitIntent *c = bro_commit_intent_new ();
  c->unit_id = x->unit_id;
  c->job_id = x->job_id;
  c->staging = g_strdup (x->staging);
  c->destination = g_strdup (x->destination);
  c->merge = x->merge;
  c->quarantine = x->quarantine;
  c->created_at = x->created_at;
  for (guint i = 0; i < x->items->len; i++)
    g_ptr_array_add (c->items, bro_commit_item_copy (x->items->pdata[i]));
  return c;
}
