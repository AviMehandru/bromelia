/* bro-sanitizer.c */
#include "bro-sanitizer.h"

#include <string.h>

char *
bro_sanitizer_component (const char *text)
{
  GString *out = g_string_new (NULL);
  for (const char *p = text; *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    if ((c < 0x80 && strchr ("/\\:*?\"<>|", (char) c) && c != 0) || c < 0x20 || (c >= 0x7F && c <= 0x9F))
      g_string_append_c (out, '-');
    else
      g_string_append_unichar (out, c);
  }
  gsize start = 0, end = out->len;
  while (start < end && (out->str[start] == ' ' || out->str[start] == '.'))
    start++;
  while (end > start && (out->str[end - 1] == ' ' || out->str[end - 1] == '.'))
    end--;
  char *s = g_strndup (out->str + start, end - start);
  g_string_free (out, TRUE);
  return s;
}
