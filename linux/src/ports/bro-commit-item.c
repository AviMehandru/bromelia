/* bro-commit-item.c */
#include "bro-commit-item.h"

BroCommitItem *
bro_commit_item_new (void)
{
  BroCommitItem *x = g_new0 (BroCommitItem, 1);
  return x;
}

void
bro_commit_item_free (BroCommitItem *x)
{
  if (!x)
    return;
  g_free (x->from_path);
  g_free (x->to_path);
  g_free (x->sha256);
  g_free (x);
}
