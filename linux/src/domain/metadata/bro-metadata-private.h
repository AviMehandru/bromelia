/* bro-metadata-private.h: helpers shared by the TMDb and OMDb builders and parsers. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

/* RFC 3986 percent-encoding: everything but letters, digits and -._~, as UTF-8. */
char *_bro_metadata_escape (const char *s);

/* The JSON object in @bytes, or NULL. */
BroJsonValue *_bro_metadata_object (GBytes *bytes);

/* A string other than OMDb's "N/A", else "". Borrowed. */
const char *_bro_metadata_text (BroJsonValue *v);

/* The year at the start of a date or a span ("1994–2004"); -1 when there is none. */
int _bro_metadata_year (BroJsonValue *v);

/* An integer that fits in an int; FALSE for anything else. */
gboolean _bro_metadata_int (BroJsonValue *v, int *out);

G_END_DECLS
