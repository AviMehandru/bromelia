/* bro-rip-mode.h: what a video-disc job makes (common.json's RipMode); audioCD and dataImage are chosen from the
 * disc's content. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_RIP_MODE_MKV,
  BRO_RIP_MODE_BACKUP,
  BRO_RIP_MODE_BACKUP_DECRYPTED,
  BRO_RIP_MODE_BACKUP_THEN_MKV,
  BRO_RIP_MODE_INFO_ONLY,
  BRO_RIP_MODE_AUDIO_CD,
  BRO_RIP_MODE_DATA_IMAGE,
} BroRipMode;

/* The wire form ("mkv"), as in shared/schema/common.json. */
const char *bro_rip_mode_to_wire (BroRipMode value);
gboolean bro_rip_mode_from_wire (const char *text, BroRipMode *out);

G_END_DECLS
