/* bro-drive-holder.h: a drive and the job it was given to. */
#pragma once

#include "bro-id.h"

G_BEGIN_DECLS

typedef struct {
  char *drive;
  BroId job;
} BroDriveHolder;

BroDriveHolder *bro_drive_holder_new (const char *drive, const BroId *job);
void bro_drive_holder_free (BroDriveHolder *holder);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDriveHolder, bro_drive_holder_free)

G_END_DECLS
