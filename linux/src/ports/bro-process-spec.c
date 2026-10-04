/* bro-process-spec.c */
#include "bro-process-spec.h"

BroProcessSpec *
bro_process_spec_new (void)
{
  BroProcessSpec *x = g_new0 (BroProcessSpec, 1);
  x->arguments = g_new0 (char *, 1);
  x->environment = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return x;
}

void
bro_process_spec_free (BroProcessSpec *x)
{
  if (!x)
    return;
  g_free (x->executable);
  g_strfreev (x->arguments);
  g_clear_pointer (&x->environment, g_hash_table_unref);
  g_free (x->working_directory);
  g_free (x->transcript);
  g_free (x->interpreter);
  g_free (x);
}
