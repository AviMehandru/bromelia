/* bro-simple-chapters.c */
#include "bro-simple-chapters.h"

#include <stdlib.h>

GArray *
bro_simple_chapters_parse (const char *text)
{
  GArray *starts = g_array_new (FALSE, FALSE, sizeof (BroDuration));
  g_autoptr (GRegex) re = g_regex_new ("^CHAPTER\\d+=(\\d+):(\\d+):([\\d.]+)", G_REGEX_MULTILINE, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  for (g_regex_match (re, text, 0, &m); g_match_info_matches (m); g_match_info_next (m, NULL)) {
    g_autofree char *h = g_match_info_fetch (m, 1), *mi = g_match_info_fetch (m, 2), *s = g_match_info_fetch (m, 3);
    BroDuration d = { atoi (h) * 3600 + atoi (mi) * 60 + g_ascii_strtod (s, NULL) };
    g_array_append_val (starts, d);
  }
  return starts;
}
