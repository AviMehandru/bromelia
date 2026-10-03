/* bro-track-kind.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_TRACK_KIND_VIDEO,
  BRO_TRACK_KIND_AUDIO,
  BRO_TRACK_KIND_SUBTITLE,
  BRO_TRACK_KIND_ATTACHMENT,
  BRO_TRACK_KIND_UNKNOWN,
} BroTrackKind;

/* The wire form ("video"), as in shared/schema/common.json. */
const char *bro_track_kind_to_wire (BroTrackKind value);
gboolean bro_track_kind_from_wire (const char *text, BroTrackKind *out);

G_END_DECLS
