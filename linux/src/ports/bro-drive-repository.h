/* bro-drive-repository.h: BroDriveRepository: drives and drive_stats_daily. */
#pragma once

#include "bro-bro-error.h"
#include "bro-drive-record.h"
#include "bro-drive-stats.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_DRIVE_REPOSITORY (bro_drive_repository_get_type ())
G_DECLARE_INTERFACE (BroDriveRepository, bro_drive_repository, BRO, DRIVE_REPOSITORY, GObject)

struct _BroDriveRepositoryInterface {
  GTypeInterface parent_iface;

  gboolean (*upsert) (BroDriveRepository *self, const BroDriveRecord *drive, BroBroError **error);
  GPtrArray *(*all) (BroDriveRepository *self, BroBroError **error);
  gboolean (*record_stats) (BroDriveRepository *self, const char *drive_id, const char *day, const BroDriveStats *stats, BroBroError **error);
};

/* FALSE and @error set on failure. */
gboolean bro_drive_repository_upsert (BroDriveRepository *self, const BroDriveRecord *drive, BroBroError **error);

GPtrArray *bro_drive_repository_all (BroDriveRepository *self, BroBroError **error);

/* Adds stats to the day's counters (day: YYYY-MM-DD). FALSE and @error set on failure. */
gboolean bro_drive_repository_record_stats (BroDriveRepository *self, const char *drive_id, const char *day, const BroDriveStats *stats, BroBroError **error);

G_END_DECLS
