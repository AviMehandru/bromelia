/* bro-selection-trace.c */
#include "bro-selection-trace.h"

BroSelectionTrace *
bro_selection_trace_new (int index, gboolean selected, BroBroMessage *reason)
{
  BroSelectionTrace *t = g_new0 (BroSelectionTrace, 1);
  t->index = index;
  t->selected = selected;
  t->reason = reason;
  return t;
}

void
bro_selection_trace_free (BroSelectionTrace *trace)
{
  if (!trace)
    return;
  bro_bro_message_free (trace->reason);
  g_free (trace);
}
