/* bro-instant.h: a UTC time with milliseconds, held as milliseconds since 1970-01-01T00:00:00Z. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gint64 unix_milliseconds;
} BroInstant;

/* RFC 3339: YYYY-MM-DDTHH:MM:SS, optional fractional seconds (cut to milliseconds), then Z or ±HH:MM. FALSE
 * for anything else. Written by hand, as on the other platforms, so all three accept exactly the same texts. */
gboolean bro_instant_parse (const char *text, BroInstant *out);

/* "2026-10-03T10:10:28.608Z". Free with g_free. */
char *bro_instant_format (BroInstant instant);

G_END_DECLS
