/* bro-home-layout.h: where makemkvcon reads settings.conf under HOME: Library/MakeMKV (macOS) or .MakeMKV (Linux). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_HOME_LAYOUT_MACOS,
  BRO_HOME_LAYOUT_LINUX,
} BroHomeLayout;

/* The wire form ("macos"). */
const char *bro_home_layout_to_wire (BroHomeLayout value);
gboolean bro_home_layout_from_wire (const char *text, BroHomeLayout *out);

G_END_DECLS
