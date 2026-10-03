/* bro-split.c */
#include "bro-split.h"

GStrv
bro_split_arguments (GArray *chapters, const char *input, const char *output)
{
  g_autoptr (GString) list = g_string_new ("chapters:");
  for (guint i = 0; i < chapters->len; i++)
    g_string_append_printf (list, "%s%d", i ? "," : "", g_array_index (chapters, int, i));
  GStrv args = g_new0 (char *, 6);
  args[0] = g_strdup ("-o");
  args[1] = g_strdup (output);
  args[2] = g_strdup ("--split");
  args[3] = g_strdup (list->str);
  args[4] = g_strdup (input);
  return args;
}
