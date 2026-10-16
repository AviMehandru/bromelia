/* bro-drive-dto.h: BroDriveDto: a drive, as clients see it (dtos.json Drive): its id, configuration entry and name,
 * MakeMKV's identification, device and index, whether it is connected, its state and disc, whether a job holds it, and
 * what its entry gives it (profile, rip on insert). The session and the countdown come with their services. */
#pragma once

#include "bro-drive-status.h"
#include "bro-id.h"
#include "bro-medium-dto.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  char *config_id; /* NULL: no entry */
  char *name;
  char *identification;
  char *device;
  gboolean has_makemkv_index;
  int makemkv_index;
  gboolean connected;
  BroDriveStatus state;
  BroMediumDto *medium; /* NULL: no disc */
  gboolean busy;
  gboolean has_job_id;
  BroId job_id;
  char *profile_id;
  gboolean rip_on_insert;
} BroDriveDto;

/* Everything zero. */
BroDriveDto *bro_drive_dto_new (void);
BroDriveDto *bro_drive_dto_copy (const BroDriveDto *drive);
void bro_drive_dto_free (BroDriveDto *drive);

/* Optional members left out when absent (makemkvIndex is null when MakeMKV hasn't numbered it). Free with
 * bro_json_value_unref. */
BroJsonValue *bro_drive_dto_json (const BroDriveDto *drive);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveDto, bro_drive_dto_free)

G_END_DECLS
