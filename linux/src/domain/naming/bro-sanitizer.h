/* bro-sanitizer.h: BroSanitizer, safe file names. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* A value made safe as one path component on every platform: / \ : * ? " < > | and control characters become '-';
 * spaces and dots are trimmed from both ends. Free with g_free. */
char *bro_sanitizer_component (const char *text);

G_END_DECLS
