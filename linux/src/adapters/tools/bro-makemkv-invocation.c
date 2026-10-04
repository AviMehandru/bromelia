/* bro-makemkv-invocation.c */
#include "bro-makemkv-invocation.h"

BroMakemkvInvocation *
bro_makemkv_invocation_new (void)
{
  BroMakemkvInvocation *x = g_new0 (BroMakemkvInvocation, 1);
  x->settings = bro_makemkv_run_settings_new ();
  x->options.min_length_seconds = -1;
  return x;
}

void
bro_makemkv_invocation_free (BroMakemkvInvocation *x)
{
  if (!x)
    return;
  bro_makemkv_run_settings_free (x->settings);
  g_free (x->transcript);
  g_free (x);
}
