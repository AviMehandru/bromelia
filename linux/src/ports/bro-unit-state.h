/* bro-unit-state.h: archive_units.state. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_UNIT_STATE_COMMITTING,
  BRO_UNIT_STATE_COMMITTED,
  BRO_UNIT_STATE_QUARANTINED,
  BRO_UNIT_STATE_MISSING,
} BroUnitState;

/* The wire form ("committing"). */
const char *bro_unit_state_to_wire (BroUnitState value);
gboolean bro_unit_state_from_wire (const char *text, BroUnitState *out);

G_END_DECLS
