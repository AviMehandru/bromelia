/* bro-severity.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_SEVERITY_DEBUG,
  BRO_SEVERITY_INFO,
  BRO_SEVERITY_WARNING,
  BRO_SEVERITY_ERROR,
} BroSeverity;

/* The wire form ("debug"), as in shared/schema/common.json. */
const char *bro_severity_to_wire (BroSeverity value);
gboolean bro_severity_from_wire (const char *text, BroSeverity *out);

G_END_DECLS
