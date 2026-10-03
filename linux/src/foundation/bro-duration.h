/* bro-duration.h: a length of time in seconds. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  double seconds;
} BroDuration;

/* makemkvcon's h:mm:ss (or m:ss, or seconds): "2:44:48" → 9888 s. A part that isn't a number counts as 0, as
 * today; FALSE when the text is empty. */
gboolean bro_duration_parse_clock (const char *text, BroDuration *out);

/* h:mm:ss, whole seconds: 9888 s → "2:44:48", 59 s → "0:00:59". Free with g_free. */
char *bro_duration_format_clock (BroDuration duration);

G_END_DECLS
