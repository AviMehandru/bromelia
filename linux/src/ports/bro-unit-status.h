/* bro-unit-status.h: archive_units.status. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_UNIT_STATUS_SUCCESS,
  BRO_UNIT_STATUS_ERRORS,
  BRO_UNIT_STATUS_INCOMPLETE,
} BroUnitStatus;

/* The wire form ("success"). */
const char *bro_unit_status_to_wire (BroUnitStatus value);
gboolean bro_unit_status_from_wire (const char *text, BroUnitStatus *out);

G_END_DECLS
