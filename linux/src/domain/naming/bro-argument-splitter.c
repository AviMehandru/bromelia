/* bro-argument-splitter.c */
#include "bro-argument-splitter.h"

#include <string.h>

GStrv
bro_argument_splitter_split (const char *s)
{
  GPtrArray *args = g_ptr_array_new ();
  GString *cur = g_string_new (NULL);
  gboolean single = FALSE, dbl = FALSE, has = FALSE;
  for (gsize i = 0; s[i]; i++) {
    char c = s[i];
    if (single) {
      if (c == '\'')
        single = FALSE;
      else
        g_string_append_c (cur, c);
    } else if (dbl) {
      if (c == '"') {
        dbl = FALSE;
      } else if (c == '\\' && s[i + 1]) {
        char n = s[++i];
        if (n == '"' || n == '\\' || n == '$' || n == '`') {
          g_string_append_c (cur, n);
        } else {
          g_string_append_c (cur, c);
          g_string_append_c (cur, n);
        }
      } else {
        g_string_append_c (cur, c);
      }
    } else if (c == '\'') {
      single = has = TRUE;
    } else if (c == '"') {
      dbl = has = TRUE;
    } else if (c == '\\' && s[i + 1]) {
      /* Keep Windows paths usable: a backslash only escapes quotes, spaces and backslashes. */
      char n = s[i + 1];
      if (n == '"' || n == '\'' || n == ' ' || n == '\\') {
        g_string_append_c (cur, n);
        i++;
      } else {
        g_string_append_c (cur, c);
      }
      has = TRUE;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      if (has || cur->len) {
        g_ptr_array_add (args, g_strdup (cur->str));
        g_string_truncate (cur, 0);
        has = FALSE;
      }
    } else {
      g_string_append_c (cur, c);
      has = TRUE;
    }
  }
  if (has || cur->len)
    g_ptr_array_add (args, g_strdup (cur->str));
  g_string_free (cur, TRUE);
  g_ptr_array_add (args, NULL);
  return (GStrv) g_ptr_array_free (args, FALSE);
}

char *
bro_argument_splitter_quote (const char *a)
{
  gboolean plain = *a != '\0';
  for (const char *p = a; *p && plain; p++)
    if (!(g_ascii_isalnum (*p) || strchr ("-_./:=+,@%", *p)))
      plain = FALSE;
  if (plain)
    return g_strdup (a);
  g_auto (GStrv) parts = g_strsplit (a, "'", -1);
  g_autofree char *joined = g_strjoinv ("'\\''", parts);
  return g_strconcat ("'", joined, "'", NULL);
}
