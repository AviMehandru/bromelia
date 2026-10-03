/* bro-duration.c */
#include "bro-duration.h"

#include <math.h>

gboolean
bro_duration_parse_clock (const char *text, BroDuration *out)
{
  if (!text)
    return FALSE;
  g_autofree char *copy = g_strdup (text);
  if (*g_strstrip (copy) == '\0')
    return FALSE;
  g_auto (GStrv) parts = g_strsplit (text, ":", -1);
  gint64 total = 0;
  for (int i = 0; parts[i]; i++) {
    char *part = g_strstrip (parts[i]);
    gint64 n = 0;
    gboolean digits = *part != '\0';
    for (char *c = part; *c; c++)
      if (!g_ascii_isdigit (*c))
        digits = FALSE;
    if (digits)
      n = g_ascii_strtoll (part, NULL, 10);
    total = total * 60 + n;
  }
  out->seconds = (double) total;
  return TRUE;
}

char *
bro_duration_format_clock (BroDuration duration)
{
  gint64 s = (gint64) floor (MAX (0, duration.seconds));
  return g_strdup_printf ("%" G_GINT64_FORMAT ":%02d:%02d", s / 3600, (int) (s / 60 % 60), (int) (s % 60));
}
