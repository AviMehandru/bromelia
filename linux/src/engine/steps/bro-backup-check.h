/* bro-backup-check.h: BroBackupCheck, whether a backup a job made looks like a disc (bro_backup_structure_problem), read
 * from disk: a folder by its files, an image by its volume descriptor at byte 32769. */
#pragma once

#include "bro-bro-message.h"
#include "bro-file-system.h"

G_BEGIN_DECLS

/* What is wrong with the backup at @path (named by its last part), or NULL: structure.notCreated when nothing is there,
 * structure.unreadable when it can't be read. Free it. */
BroBroMessage *bro_backup_check_problem (BroFileSystem *fs, const char *path);

G_END_DECLS
