/* bro-makemkv-run-settings.c */
#include "bro-makemkv-run-settings.h"

BroMakemkvRunSettings *
bro_makemkv_run_settings_new (void)
{
  BroMakemkvRunSettings *x = g_new0 (BroMakemkvRunSettings, 1);
  x->settings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return x;
}

void
bro_makemkv_run_settings_free (BroMakemkvRunSettings *x)
{
  if (!x)
    return;
  g_clear_pointer (&x->settings, g_hash_table_unref);
  g_free (x->profile_xml);
  g_free (x->data_dir);
  g_free (x);
}
