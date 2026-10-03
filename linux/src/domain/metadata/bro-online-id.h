/* bro-online-id.h: a movie or show chosen by its id: a TMDb id (603, tmdb:603, movie/603, tv/1668, a themoviedb.org
 * address; no kind when it doesn't say) or an IMDb id (tt0133093, an imdb.com address). */
#pragma once

#include "bro-media-kind.h"
#include "bro-name-and-year.h"

G_BEGIN_DECLS

typedef enum {
  BRO_ONLINE_ID_TMDB, /* tmdb_id, and media_kind when has_media_kind */
  BRO_ONLINE_ID_IMDB, /* imdb_id */
} BroOnlineIdKind;

typedef struct {
  BroOnlineIdKind kind;
  int tmdb_id;
  gboolean has_media_kind;
  BroMediaKind media_kind;
  char *imdb_id;
} BroOnlineId;

void bro_online_id_free (BroOnlineId *id);

/* The id in @text (case and surrounding space ignored); NULL when there is none. */
BroOnlineId *bro_online_id_parse (const char *text);

/* A year typed after the name: "Inception (2010)" → Inception, 2010 (years 1870 to 2100); otherwise the trimmed text
 * and no year. */
BroNameAndYear *bro_online_id_split_year (const char *text);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroOnlineId, bro_online_id_free)

G_END_DECLS
