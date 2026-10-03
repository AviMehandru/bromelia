/* bro-online-id.c */
#include "bro-online-id.h"

#include <stdlib.h>
#include <string.h>

void
bro_name_and_year_free (BroNameAndYear *value)
{
  if (!value)
    return;
  g_free (value->name);
  g_free (value);
}

void
bro_online_id_free (BroOnlineId *id)
{
  if (!id)
    return;
  g_free (id->imdb_id);
  g_free (id);
}

/* The groups of the first match of @pattern in @text, or NULL (free with g_strfreev). Unmatched groups are "". */
static GStrv
first_match (const char *pattern, const char *text)
{
  g_autoptr (GRegex) re = g_regex_new (pattern, 0, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  if (!g_regex_match (re, text, 0, &m))
    return NULL;
  return g_match_info_fetch_all (m);
}

static BroOnlineId *
tmdb (const char *digits, const char *kind)
{
  /* At most nine digits (the patterns say so), so no overflow. */
  long n = strtol (digits, NULL, 10);
  if (n <= 0)
    return NULL;
  BroOnlineId *id = g_new0 (BroOnlineId, 1);
  id->kind = BRO_ONLINE_ID_TMDB;
  id->tmdb_id = (int) n;
  id->has_media_kind = kind && *kind;
  id->media_kind = kind && strcmp (kind, "tv") == 0 ? BRO_MEDIA_KIND_TV : BRO_MEDIA_KIND_MOVIE;
  return id;
}

BroOnlineId *
bro_online_id_parse (const char *text)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (text));
  g_autofree char *t = g_ascii_strdown (trimmed, -1);
  g_auto (GStrv) imdb = first_match ("tt\\d{5,10}", t);
  if (imdb) {
    BroOnlineId *id = g_new0 (BroOnlineId, 1);
    id->kind = BRO_ONLINE_ID_IMDB;
    id->imdb_id = g_strdup (imdb[0]);
    return id;
  }
  g_auto (GStrv) url = first_match ("themoviedb\\.org/(movie|tv)/(\\d+)", t);
  if (url)
    return strlen (url[2]) <= 9 ? tmdb (url[2], url[1]) : NULL;
  g_auto (GStrv) m = first_match ("^(?:tmdb:)?(?:(movie|tv)/)?(\\d{1,9})$", t);
  return m ? tmdb (m[2], m[1]) : NULL;
}

BroNameAndYear *
bro_online_id_split_year (const char *text)
{
  BroNameAndYear *r = g_new0 (BroNameAndYear, 1);
  g_autofree char *t = g_strstrip (g_strdup (text));
  g_auto (GStrv) m = first_match ("^(.*\\S)\\s*\\((\\d{4})\\)$", t);
  int y = m ? atoi (m[2]) : 0;
  if (m && y >= 1870 && y <= 2100) {
    r->name = g_strdup (m[1]);
    r->year = y;
  } else {
    r->name = g_strdup (t);
    r->year = -1;
  }
  return r;
}
