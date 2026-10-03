/* bro-candidate.h: a movie or show found online: title, year, TMDb and IMDb ids, the provider ("TMDb" or "OMDb"),
 * movie or TV as listed, plot and poster URL ("" when there is none). */
#pragma once

#include "bro-media-kind.h"

G_BEGIN_DECLS

typedef struct {
  char *title;
  int year;     /* -1: none */
  int tmdb_id;  /* -1: none */
  char *imdb_id; /* nullable */
  char *provider;
  gboolean has_kind;
  BroMediaKind kind;
  char *overview;
  char *poster;
} BroCandidate;

/* No year, ids or kind; empty plot and poster. */
BroCandidate *bro_candidate_new (const char *title, const char *provider);
BroCandidate *bro_candidate_copy (const BroCandidate *candidate);
void bro_candidate_free (BroCandidate *candidate);

/* What to type or pick to choose this candidate: movie/603, tv/1668 or tt0133093. */
char *bro_candidate_choice (const BroCandidate *candidate);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCandidate, bro_candidate_free)

G_END_DECLS
