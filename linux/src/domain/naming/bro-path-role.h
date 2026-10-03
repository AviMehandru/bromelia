/* bro-path-role.h: what a planned path holds (the API's PlannedPath role) */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_PATH_ROLE_TITLE,
  BRO_PATH_ROLE_EPISODE,
  BRO_PATH_ROLE_CLOSING_CLIP,
  BRO_PATH_ROLE_PLAY_ALL,
  BRO_PATH_ROLE_BACKUP,
  BRO_PATH_ROLE_IMAGE,
  BRO_PATH_ROLE_AUDIO,
  BRO_PATH_ROLE_DISC_INFO,
  BRO_PATH_ROLE_RECORD,
  BRO_PATH_ROLE_SUMS,
  BRO_PATH_ROLE_LOG,
  BRO_PATH_ROLE_NFO,
  BRO_PATH_ROLE_POSTER,
} BroPathRole;

/* The wire form ("title"), as in shared/schema/common.json. */
const char *bro_path_role_to_wire (BroPathRole value);
gboolean bro_path_role_from_wire (const char *text, BroPathRole *out);

G_END_DECLS
