/* bro-status-word.h: the status given to user scripts (BROMELIA_STATUS), notifications and the manifest */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_STATUS_WORD_SUCCESS,
  BRO_STATUS_WORD_ERRORS,
  BRO_STATUS_WORD_FAILED,
  BRO_STATUS_WORD_CANCELLED,
  BRO_STATUS_WORD_SKIPPED,
  BRO_STATUS_WORD_INTERRUPTED,
} BroStatusWord;

/* The wire form ("success"), as in shared/schema/common.json. */
const char *bro_status_word_to_wire (BroStatusWord value);
gboolean bro_status_word_from_wire (const char *text, BroStatusWord *out);

G_END_DECLS
