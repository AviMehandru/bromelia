/* bro-beta-key.c */
#include "bro-beta-key.h"

gboolean
bro_beta_key_may_replace (const char *current_key)
{
  if (!current_key)
    return TRUE;
  g_autofree char *k = g_strstrip (g_strdup (current_key));
  return !*k || g_str_has_prefix (k, "T-");
}
