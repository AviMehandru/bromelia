/* bro-makemkv-run.c */
#include "bro-makemkv-run.h"

BroMakemkvRun *
bro_makemkv_run_new (void)
{
  return g_new0 (BroMakemkvRun, 1);
}

void
bro_makemkv_run_free (BroMakemkvRun *x)
{
  if (!x)
    return;
  g_clear_pointer (&x->outcome, bro_run_outcome_free);
  g_clear_pointer (&x->notice, bro_makemkv_notice_free);
  g_free (x->libre_drive);
  g_free (x->version);
  g_free (x);
}
