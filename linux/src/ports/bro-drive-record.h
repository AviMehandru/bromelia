/* bro-drive-record.h: A row of drives. */
#pragma once

#include "bro-instant.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *id; /* DriveId */
  char *identification;
  char *model;
  char *last_device;
  char *config_id; /* nullable */
  BroInstant first_seen_at;
  BroInstant last_seen_at;
} BroDriveRecord;

/* Everything zero. */
BroDriveRecord *bro_drive_record_new (void);
void bro_drive_record_free (BroDriveRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveRecord, bro_drive_record_free)

G_END_DECLS
