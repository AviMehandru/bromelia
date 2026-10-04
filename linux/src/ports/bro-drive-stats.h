/* bro-drive-stats.h: A day's counters of drive_stats_daily. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int jobs;
  int failed_jobs;
  int read_error_jobs;
  int read_errors;
  gint64 bytes_read;
  double seconds_reading;
} BroDriveStats;

/* Everything zero. */
BroDriveStats *bro_drive_stats_new (void);
void bro_drive_stats_free (BroDriveStats *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveStats, bro_drive_stats_free)

G_END_DECLS
