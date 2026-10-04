/* bro-nfo.c */
#include "bro-nfo.h"

#include <string.h>

/* One element when @value isn't empty; @name may carry attributes. */
static void
element (GString *s, const char *name, const char *value)
{
  if (!value || !*value)
    return;
  g_string_append_printf (s, "  <%s>", name);
  for (const char *p = value; *p; p++)
    switch (*p) {
    case '&': g_string_append (s, "&amp;"); break;
    case '<': g_string_append (s, "&lt;"); break;
    case '>': g_string_append (s, "&gt;"); break;
    default: g_string_append_c (s, *p);
    }
  g_string_append_printf (s, "</%.*s>\n", (int) strcspn (name, " "), name);
}

static GString *
start (const char *root)
{
  GString *s = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
  g_string_append_printf (s, "<%s>\n", root);
  return s;
}

static char *
finish (GString *s, const char *root)
{
  g_string_append_printf (s, "</%s>\n", root);
  return g_string_free (s, FALSE);
}

/* Title, year, plot and the ids (the first one is the default). */
static char *
titled (const char *root, const BroCandidate *m)
{
  GString *s = start (root);
  element (s, "title", m->title);
  g_autofree char *year = m->year >= 0 ? g_strdup_printf ("%d", m->year) : NULL;
  element (s, "year", year);
  element (s, "plot", m->overview);
  gboolean first = TRUE;
  if (m->tmdb_id >= 0) {
    g_autofree char *id = g_strdup_printf ("%d", m->tmdb_id);
    element (s, "uniqueid type=\"tmdb\" default=\"true\"", id);
    first = FALSE;
  }
  if (m->imdb_id && *m->imdb_id)
    element (s, first ? "uniqueid type=\"imdb\" default=\"true\"" : "uniqueid type=\"imdb\"", m->imdb_id);
  return finish (s, root);
}

char *
bro_nfo_movie (const BroCandidate *match)
{
  return titled ("movie", match);
}

char *
bro_nfo_show (const BroCandidate *match)
{
  return titled ("tvshow", match);
}

char *
bro_nfo_episode (const char *show, int season, int episode, const BroEpisodeDetails *details)
{
  GString *s = start ("episodedetails");
  g_autofree char *se = g_strdup_printf ("%d", season), *ep = g_strdup_printf ("%d", episode);
  element (s, "title", details->title);
  element (s, "showtitle", show);
  element (s, "season", se);
  element (s, "episode", ep);
  element (s, "plot", details->plot);
  element (s, "aired", details->aired);
  return finish (s, "episodedetails");
}
