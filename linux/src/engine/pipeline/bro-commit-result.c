/* bro-commit-result.c */
#include "bro-commit-result.h"

BroCommitResult *
bro_commit_result_new (BroId unit_id, const char *destination, GPtrArray *files, gboolean merged)
{
  BroCommitResult *r = g_new0 (BroCommitResult, 1);
  r->unit_id = unit_id;
  r->destination = g_strdup (destination);
  r->files = files;
  r->merged = merged;
  return r;
}

void
bro_commit_result_free (BroCommitResult *r)
{
  if (!r)
    return;
  g_free (r->destination);
  g_clear_pointer (&r->files, g_ptr_array_unref);
  g_free (r);
}
