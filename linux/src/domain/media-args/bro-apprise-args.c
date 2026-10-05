/* bro-apprise-args.c */
#include "bro-apprise-args.h"

GStrv
bro_apprise_args_build (const char *title, const char *body)
{
  GStrv args = g_new0 (char *, 5);
  args[0] = g_strdup ("-t");
  args[1] = g_strdup (title);
  args[2] = g_strdup ("-b");
  args[3] = g_strdup (body);
  return args;
}

GHashTable *
bro_apprise_args_environment (const char *url)
{
  GHashTable *env = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (env, g_strdup ("APPRISE_URLS"), g_strdup (url));
  return env;
}
