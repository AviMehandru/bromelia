/* bro-disc-set-record.h: A row of disc_sets: the discs of a release that belong together. */
#pragma once

#include "bro-id.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId id;
  gboolean has_work_id;
  BroId work_id;
  char *label_title;
  gboolean has_season;
  int season;
  gboolean has_part;
  int part;
  gboolean has_volume;
  int volume;
  char *description;
  gboolean has_known_count;
  int known_count;
} BroDiscSetRecord;

/* Everything zero. */
BroDiscSetRecord *bro_disc_set_record_new (void);
void bro_disc_set_record_free (BroDiscSetRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDiscSetRecord, bro_disc_set_record_free)

G_END_DECLS
