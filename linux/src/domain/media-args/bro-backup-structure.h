/* bro-backup-structure.h: BroBackupStructure, whether a backup looks like a disc: a folder with a BDMV, VIDEO_TS or
 * HVDVD_TS structure, or an ISO image (an ISO 9660 or UDF volume descriptor at byte 32769). */
#pragma once

#include "bro-bro-message.h"

G_BEGIN_DECLS

/* What is wrong with the backup called @name, or NULL. @entries lists the folder's files as relative paths
 * (NULL-terminated; a trailing / is an empty folder; NULL: not a folder); @iso_header is the file's bytes from 32769
 * (at most 5; NULL: not a file). */
BroBroMessage *bro_backup_structure_problem (const char *name, gboolean iso, const char *const *entries, GBytes *iso_header);

G_END_DECLS
