/* bro-tool-kind.h: the external tools Bromelia runs. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_TOOL_KIND_MAKEMKVCON,
  BRO_TOOL_KIND_MKVMERGE,
  BRO_TOOL_KIND_MKVEXTRACT,
  BRO_TOOL_KIND_FFMPEG,
  BRO_TOOL_KIND_TESSERACT,
  BRO_TOOL_KIND_HANDBRAKE,
  BRO_TOOL_KIND_CYANRIP,
  BRO_TOOL_KIND_ABCDE,
  BRO_TOOL_KIND_PAR2,
  BRO_TOOL_KIND_APPRISE,
} BroToolKind;

/* The wire form ("makemkvcon"). */
const char *bro_tool_kind_to_wire (BroToolKind value);
gboolean bro_tool_kind_from_wire (const char *text, BroToolKind *out);

G_END_DECLS
