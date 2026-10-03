/* bro-apprise-args.c */
#include "bro-apprise-args.h"

GStrv
bro_apprise_args_build (const char *url, const char *title, const char *body)
{
  GStrv args = g_new0 (char *, 6);
  args[0] = g_strdup ("-t");
  args[1] = g_strdup (title);
  args[2] = g_strdup ("-b");
  args[3] = g_strdup (body);
  args[4] = g_strdup (url);
  return args;
}
