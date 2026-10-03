/* bro-candidate.c */
#include "bro-candidate.h"

BroCandidate *
bro_candidate_new (const char *title, const char *provider)
{
  BroCandidate *c = g_new0 (BroCandidate, 1);
  c->title = g_strdup (title);
  c->year = -1;
  c->tmdb_id = -1;
  c->provider = g_strdup (provider);
  c->overview = g_strdup ("");
  c->poster = g_strdup ("");
  return c;
}

BroCandidate *
bro_candidate_copy (const BroCandidate *candidate)
{
  BroCandidate *c = g_new0 (BroCandidate, 1);
  *c = *candidate;
  c->title = g_strdup (candidate->title);
  c->imdb_id = g_strdup (candidate->imdb_id);
  c->provider = g_strdup (candidate->provider);
  c->overview = g_strdup (candidate->overview);
  c->poster = g_strdup (candidate->poster);
  return c;
}

void
bro_candidate_free (BroCandidate *candidate)
{
  if (!candidate)
    return;
  g_free (candidate->title);
  g_free (candidate->imdb_id);
  g_free (candidate->provider);
  g_free (candidate->overview);
  g_free (candidate->poster);
  g_free (candidate);
}

char *
bro_candidate_choice (const BroCandidate *candidate)
{
  if (candidate->tmdb_id >= 0)
    return g_strdup_printf ("%s/%d", candidate->has_kind && candidate->kind == BRO_MEDIA_KIND_TV ? "tv" : "movie", candidate->tmdb_id);
  return g_strdup (candidate->imdb_id ? candidate->imdb_id : "");
}
