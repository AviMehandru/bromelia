/* bro-unit-record.h: A row of archive_units: one archived acquisition of a disc. */
#pragma once

#include "bro-id.h"
#include "bro-instant.h"
#include "bro-media-kind.h"
#include "bro-unit-state.h"
#include "bro-unit-status.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId id;
  char *library_id;
  gboolean has_physical_disc_id;
  BroId physical_disc_id;
  gboolean has_job_id;
  BroId job_id;
  char *path; /* relative to the library */
  char *record_file;
  int record_version;
  BroUnitState state;
  BroUnitStatus status;
  char *name;
  BroMediaKind kind;
  char *format;
  char *format_code;
  gboolean encrypted;
  char *fingerprint; /* nullable */
  char *label;
  gboolean has_season;
  int season;
  gboolean has_part;
  int part;
  gboolean has_volume;
  int volume;
  gboolean has_disc;
  int disc;
  char *makemkv_version;
  gint64 bytes;
  int file_count;
  int attempts;
  BroInstant created_at;
  gboolean has_committed_at;
  BroInstant committed_at;
} BroUnitRecord;

/* Everything zero. */
BroUnitRecord *bro_unit_record_new (void);
void bro_unit_record_free (BroUnitRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroUnitRecord, bro_unit_record_free)

G_END_DECLS
