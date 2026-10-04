/* bro-work-record.h: A row of works: a movie or show. */
#pragma once

#include "bro-id.h"
#include "bro-instant.h"
#include "bro-media-kind.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId id;
  BroMediaKind kind;
  char *title;
  gboolean has_year;
  int year;
  gboolean has_tmdb_id;
  int tmdb_id;
  char *imdb_id; /* nullable */
  BroInstant created_at;
  BroInstant updated_at;
} BroWorkRecord;

/* Everything zero. */
BroWorkRecord *bro_work_record_new (void);
void bro_work_record_free (BroWorkRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroWorkRecord, bro_work_record_free)

G_END_DECLS
