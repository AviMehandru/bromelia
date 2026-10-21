/* bro-named-file.h: BroNamedFile, an item a job made, by its final name: a file or a folder (a backup) relative to the
 * job's staging folder, which is the unit's folder as it will be in the library; its role in the archive record (title,
 * episode, closingClip, playAll, backup, image, audio, discInfo; NULL for a file no title owns) and the title it is of. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  char *path;
  char *role; /* nullable */
  gboolean has_title;
  int title;
} BroNamedFile;

/* @role nullable; @title < 0: none. */
BroNamedFile *bro_named_file_new (const char *path, const char *role, int title);
BroNamedFile *bro_named_file_copy (const BroNamedFile *file);
void bro_named_file_free (BroNamedFile *file);

/* {"path", "role", "title"}; role and title left out when there is none. */
BroJsonValue *bro_named_file_encode (const BroNamedFile *file);
BroNamedFile *bro_named_file_decode (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroNamedFile, bro_named_file_free)

G_END_DECLS
