/* bro-label-parser.c */
#include "bro-label-parser.h"

#include <stdlib.h>
#include <string.h>

static const char *const noise[] = { "WS", "FS", "16X9", "4X3", "NTSC", "PAL", "R1", "R2", "R4", "UHD", "4K", "BD", "BLURAY", "BLU",
                                     "RAY", "DVD", "DVD5", "DVD9", "BD25", "BD50", "BD66", "BD100", "HDR", "SDR", "HD", "DISC", "DISK",
                                     NULL };
static const char *const small_words[] = { "a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into", "nor", "of",
                                           "on", "or", "the", "to", "vs", "with", NULL };

static gboolean
in_list (const char *const *list, const char *word)
{
  for (int i = 0; list[i]; i++)
    if (strcmp (list[i], word) == 0)
      return TRUE;
  return FALSE;
}

static char *
replace (const char *s, const char *what, const char *with)
{
  g_auto (GStrv) parts = g_strsplit (s, what, -1);
  return g_strjoinv (with, parts);
}

/* The groups of an anchored match of @pattern on @s ("" for a group that didn't take part), or NULL. */
static GStrv
match (const char *pattern, const char *s)
{
  g_autoptr (GRegex) re = g_regex_new (pattern, 0, 0, NULL);
  g_autoptr (GMatchInfo) info = NULL;
  if (!g_regex_match (re, s, 0, &info))
    return NULL;
  int n = g_match_info_get_match_count (info);
  GStrv groups = g_new0 (char *, 4);
  for (int g = 1; g < 4; g++)
    groups[g - 1] = g < n ? g_match_info_fetch (info, g) : g_strdup ("");
  return groups;
}

static gboolean
digits_only (const char *s, int *out)
{
  if (!*s)
    return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  *out = atoi (s);
  return TRUE;
}

static gboolean
utf8_equal_case (const char *w, gboolean upper)
{
  g_autofree char *c = upper ? g_utf8_strup (w, -1) : g_utf8_strdown (w, -1);
  return strcmp (c, w) == 0;
}

/* Title-cases words when the label is all upper or all lower case; mixed case is kept. */
static char *
title_case (GPtrArray *words)
{
  gboolean all_upper = TRUE, all_lower = TRUE;
  for (guint i = 0; i < words->len; i++) {
    all_upper = all_upper && utf8_equal_case (words->pdata[i], TRUE);
    all_lower = all_lower && utf8_equal_case (words->pdata[i], FALSE);
  }
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < words->len; i++) {
    const char *w = words->pdata[i];
    if (i)
      g_string_append_c (out, ' ');
    if (!all_upper && !all_lower) {
      g_string_append (out, w);
      continue;
    }
    g_autofree char *lower = g_utf8_strdown (w, -1);
    if (i > 0 && in_list (small_words, lower)) {
      g_string_append (out, lower);
      continue;
    }
    gboolean roman = g_utf8_strlen (w, -1) <= 4;
    for (const char *p = lower; *p && roman; p++)
      roman = *p == 'i' || *p == 'v' || *p == 'x';
    if (roman) {
      g_autofree char *up = g_utf8_strup (w, -1);
      g_string_append (out, up);
      continue;
    }
    g_auto (GStrv) pieces = g_strsplit (lower, "-", -1);
    for (int k = 0; pieces[k]; k++) {
      if (k)
        g_string_append_c (out, '-');
      if (!pieces[k][0])
        continue;
      const char *rest = g_utf8_next_char (pieces[k]);
      g_autofree char *first = g_strndup (pieces[k], rest - pieces[k]);
      g_autofree char *first_up = g_utf8_strup (first, -1);
      g_string_append (out, first_up);
      g_string_append (out, rest);
    }
  }
  return g_string_free (out, FALSE);
}

BroLabel *
bro_label_parser_parse (const char *label)
{
  BroLabel *l = g_new0 (BroLabel, 1);
  l->season = l->part = l->volume = l->disc = -1;
  g_autofree char *a = replace (label, "™", "");
  g_autofree char *b = replace (a, "®", "");
  g_autofree char *c = replace (b, "©", "");
  g_autofree char *s = replace (c, " - ", " ");
  g_auto (GStrv) raw = g_strsplit_set (s, "_. \t()[],", -1);
  g_autoptr (GPtrArray) tokens = g_ptr_array_new ();
  for (int i = 0; raw[i]; i++)
    if (raw[i][0] && strcmp (raw[i], "-") != 0)
      g_ptr_array_add (tokens, raw[i]);
  g_autoptr (GPtrArray) title = g_ptr_array_new ();
  gboolean stopped = FALSE;
  for (guint i = 0; i < tokens->len; i++) {
    const char *t = tokens->pdata[i];
    g_autofree char *u = g_ascii_strup (t, -1);
    gboolean marker = TRUE, consumed = FALSE;
    int next = 0;
    gboolean has_next = i + 1 < tokens->len && digits_only (tokens->pdata[i + 1], &next);
    g_auto (GStrv) g = NULL;
    if ((g = match ("^S([0-9]{1,2})(?:D([0-9]{1,2}))?(?:E([0-9]{1,3}))?$", u))) {
      l->season = atoi (g[0]);
      if (g[1][0])
        l->disc = atoi (g[1]);
      l->looks_like_series = TRUE;
    } else if ((g = match ("^SEASON([0-9]{1,2})?$", u))) {
      if (g[0][0])
        l->season = atoi (g[0]);
      else if (has_next) {
        l->season = next;
        consumed = TRUE;
      }
      l->looks_like_series = TRUE;
    } else if ((g = match ("^(?:EP|EPS|EPISODE|EPISODES)([0-9]{1,3})?$", u))) {
      if (!g[0][0] && has_next)
        consumed = TRUE;
      l->looks_like_series = TRUE;
    } else if ((g = match ("^(?:D|DISC|DISK|CD)([0-9]{1,2})$", u))) {
      l->disc = atoi (g[0]);
    } else if ((strcmp (u, "DISC") == 0 || strcmp (u, "DISK") == 0 || strcmp (u, "D") == 0) && has_next) {
      l->disc = next;
      consumed = TRUE;
    } else if ((g = match ("^(?:P|PT|PART)([0-9]{1,2})$", u))) {
      l->part = atoi (g[0]);
    } else if ((strcmp (u, "PART") == 0 || strcmp (u, "PT") == 0) && has_next) {
      l->part = next;
      consumed = TRUE;
    } else if ((g = match ("^(?:V|VOL|VOLUME)([0-9]{1,2})$", u))) {
      l->volume = atoi (g[0]);
      l->looks_like_series = TRUE;
    } else if ((strcmp (u, "VOL") == 0 || strcmp (u, "VOLUME") == 0) && has_next) {
      l->volume = next;
      consumed = TRUE;
      l->looks_like_series = TRUE;
    } else {
      marker = FALSE;
    }
    if (marker)
      stopped = TRUE;
    else if (!stopped && !in_list (noise, u))
      g_ptr_array_add (title, (gpointer) t);
    if (consumed)
      i++;
  }
  l->title = title_case (title);
  return l;
}

char *
bro_label_parser_set_description (const BroLabel *label)
{
  GString *s = g_string_new (NULL);
  const char *names[] = { "Season", "Part", "Volume", "Disc" };
  int values[] = { label->season, label->part, label->volume, label->disc };
  for (int i = 0; i < 4; i++)
    if (values[i] >= 0)
      g_string_append_printf (s, "%s%s %d", s->len ? " " : "", names[i], values[i]);
  return g_string_free (s, FALSE);
}
