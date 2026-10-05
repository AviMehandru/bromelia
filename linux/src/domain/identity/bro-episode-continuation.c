/* bro-episode-continuation.c */
#include "bro-episode-continuation.h"
#include "bro-identity-private.h"

#include <stdlib.h>
#include <string.h>

char *
_bro_identity_normalize_name (const char *name)
{
  GString *out = g_string_new (NULL);
  for (const char *p = name; p && *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    switch (g_unichar_type (c)) {
    case G_UNICODE_UPPERCASE_LETTER:
    case G_UNICODE_LOWERCASE_LETTER:
    case G_UNICODE_TITLECASE_LETTER:
    case G_UNICODE_MODIFIER_LETTER:
    case G_UNICODE_OTHER_LETTER:
    case G_UNICODE_DECIMAL_NUMBER:
      g_string_append_unichar (out, g_unichar_tolower (c));
      break;
    default:
      break;
    }
  }
  return g_string_free (out, FALSE);
}

gboolean
bro_episode_continuation_same_set (const BroArchivedDisc *disc, const BroContinuationQuery *query)
{
  g_autofree char *name = _bro_identity_normalize_name (query->name);
  g_autofree char *title = _bro_identity_normalize_name (query->label_title);
  g_autofree char *disc_name = _bro_identity_normalize_name (disc->name);
  g_autofree char *disc_title = _bro_identity_normalize_name (disc->label_title);
  gboolean same_show = (name[0] && strcmp (disc_name, name) == 0) || (title[0] && strcmp (disc_title, title) == 0);
  return same_show && disc->season == query->season && disc->part == query->part && disc->volume == query->volume;
}

BroPreviousEpisode *
bro_episode_continuation_choose (const BroContinuationQuery *q, GPtrArray *candidates)
{
  if (q->disc <= 1)
    return NULL;
  const BroArchivedDisc *prev = NULL;
  gboolean later = FALSE;
  for (guint i = 0; i < candidates->len; i++) {
    const BroArchivedDisc *r = candidates->pdata[i];
    if (!bro_episode_continuation_same_set (r, q))
      continue;
    if (r->disc == q->disc - 1 && r->last_episode >= 0 && (!prev || r->last_episode > prev->last_episode))
      prev = r;
    if (r->disc > q->disc)
      later = TRUE;
  }
  BroPreviousEpisode *found = NULL;
  if (prev) {
    found = g_new0 (BroPreviousEpisode, 1);
    found->last_episode = prev->last_episode;
    found->source = g_strdup (prev->folder);
  } else if (!later && q->season_folder_highest >= 0) {
    found = g_new0 (BroPreviousEpisode, 1);
    found->last_episode = q->season_folder_highest;
    found->source = g_strdup (q->season_folder ? q->season_folder : "");
  }
  return found;
}

gboolean
bro_episode_continuation_highest_in_season (const char *const *file_names, int season, int *out)
{
  g_autoptr (GRegex) re = g_regex_new ("[Ss]([0-9]{1,3})[Ee]([0-9]{1,4})", 0, 0, NULL);
  gboolean found = FALSE;
  for (int i = 0; file_names && file_names[i]; i++) {
    if (file_names[i][0] == '.')
      continue;
    g_autoptr (GMatchInfo) info = NULL;
    if (!g_regex_match (re, file_names[i], 0, &info))
      continue;
    g_autofree char *s = g_match_info_fetch (info, 1);
    g_autofree char *e = g_match_info_fetch (info, 2);
    if (atoi (s) != season)
      continue;
    if (!found || atoi (e) > *out)
      *out = atoi (e);
    found = TRUE;
  }
  return found;
}
