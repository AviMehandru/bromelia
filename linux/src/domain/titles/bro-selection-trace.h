/* bro-selection-trace.h: why one title is in or out of a selection. */
#pragma once

#include "bro-bro-message.h"

G_BEGIN_DECLS

typedef struct {
  int index;
  gboolean selected;
  BroBroMessage *reason;
} BroSelectionTrace;

/* Takes ownership of @reason. */
BroSelectionTrace *bro_selection_trace_new (int index, gboolean selected, BroBroMessage *reason);
void bro_selection_trace_free (BroSelectionTrace *trace);

G_END_DECLS
