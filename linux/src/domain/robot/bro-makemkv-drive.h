/* bro-makemkv-drive.h: a drive as MakeMKV reports it (a DRV line): its index for disc:N, state, disc flags,
 * identification (model, firmware, usually serial), disc label and OS device. */
#pragma once

#include "bro-disc-flags.h"
#include "bro-drive-state.h"
#include "bro-robot-event.h"

G_BEGIN_DECLS

typedef struct {
  int index;
  BroDriveState state;
  BroDiscFlags flags;
  char *identification;
  char *label;
  char *device;
} BroMakemkvDrive;

BroMakemkvDrive *bro_makemkv_drive_new (int index, BroDriveState state, int flags, const char *identification,
                                        const char *label, const char *device);
BroMakemkvDrive *bro_makemkv_drive_copy (const BroMakemkvDrive *drive);
void bro_makemkv_drive_free (BroMakemkvDrive *drive);

/* NULL unless @event is a DRV event. */
BroMakemkvDrive *bro_makemkv_drive_from (const BroRobotEvent *event);

/* The drive is there: its state isn't noDrive, and it has a name or a device. */
gboolean bro_makemkv_drive_is_present (const BroMakemkvDrive *drive);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMakemkvDrive, bro_makemkv_drive_free)

G_END_DECLS
