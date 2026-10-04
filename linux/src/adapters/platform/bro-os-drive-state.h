/* bro-os-drive-state.h: BroOsDriveState: an optical drive as the operating system sees it at one moment, and whether
 * it holds media (shared/fixtures/adapters/os-drive-states.cases.json). */
#pragma once

#include "bro-device-event.h"
#include "bro-os-drive.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroOsDrive *drive;
  gboolean media;
} BroOsDriveState;

/* Takes @drive. */
BroOsDriveState *bro_os_drive_state_new (BroOsDrive *drive, gboolean media);
void bro_os_drive_state_free (BroOsDriveState *state);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroOsDriveState, bro_os_drive_state_free)

/* The events (BroDeviceEvent *) between two snapshots (BroOsDriveState *): for each drive of @after, appeared or what
 * changed (unmounted, media removed, media arrived, mounted); then each drive that is gone (unmounted, media removed,
 * vanished). */
GPtrArray *bro_os_drive_state_changes (GPtrArray *before, GPtrArray *after);

G_END_DECLS
