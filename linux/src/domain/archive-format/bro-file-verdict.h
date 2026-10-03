/* bro-file-verdict.h: what hashing one listed file found */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_FILE_VERDICT_SAME,
  BRO_FILE_VERDICT_CHANGED,
  BRO_FILE_VERDICT_MISSING,
  BRO_FILE_VERDICT_UNREADABLE,
} BroFileVerdict;

/* The wire form ("same"), as in shared/schema/common.json. */
const char *bro_file_verdict_to_wire (BroFileVerdict value);
gboolean bro_file_verdict_from_wire (const char *text, BroFileVerdict *out);

G_END_DECLS
