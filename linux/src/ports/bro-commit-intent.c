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
