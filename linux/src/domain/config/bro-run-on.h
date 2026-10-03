/* bro-run-on.h: when a step runs: success; failure (failed, cancelled, interrupted, or read errors); always */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_RUN_ON_SUCCESS,
  BRO_RUN_ON_FAILURE,
  BRO_RUN_ON_ALWAYS,
} BroRunOn;

/* The wire form ("success"), as in shared/schema/common.json. */
const char *bro_run_on_to_wire (BroRunOn value);
gboolean bro_run_on_from_wire (const char *text, BroRunOn *out);

G_END_DECLS
