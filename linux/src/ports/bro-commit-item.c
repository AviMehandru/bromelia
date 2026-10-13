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

BroCommitItem *
bro_commit_item_copy (const BroCommitItem *x)
{
  BroCommitItem *c = g_new0 (BroCommitItem, 1);
  *c = *x;
  c->from_path = g_strdup (x->from_path);
  c->to_path = g_strdup (x->to_path);
  c->sha256 = g_strdup (x->sha256);
  return c;
}
