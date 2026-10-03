/* bro-joined-drive.h: a drive as the engine knows it: its id, what MakeMKV reported and what the OS reported
 * (either may be missing). */
#pragma once

#include "bro-makemkv-drive.h"
#include "bro-os-drive.h"

G_BEGIN_DECLS

typedef struct {
  char *drive_id;
  BroMakemkvDrive *makemkv; /* nullable */
  BroOsDrive *os;           /* nullable */
} BroJoinedDrive;

/* Takes ownership of @makemkv and @os. */
BroJoinedDrive *bro_joined_drive_new (const char *drive_id, BroMakemkvDrive *makemkv, BroOsDrive *os);
void bro_joined_drive_free (BroJoinedDrive *drive);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJoinedDrive, bro_joined_drive_free)

G_END_DECLS
