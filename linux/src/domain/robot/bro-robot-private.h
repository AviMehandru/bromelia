/* bro-robot-private.h: helpers shared by the robot module's files (internal in Swift and C# too). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* An integer field: optional sign and ASCII digits, surrounding spaces allowed. */
gboolean _bro_robot_parse_int (const char *text, int *out);

/* ASCII lower case (MakeMKV's texts and device names); free with g_free. */
char *_bro_robot_ascii_lower (const char *text);

G_END_DECLS
