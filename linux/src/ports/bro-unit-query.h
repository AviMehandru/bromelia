/* bro-unit-query.h: which units: of a library, matching a name, in a state; a page at a time. */
#pragma once

#include "bro-unit-state.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *library_id; /* nullable */
  char *text; /* nullable */
  gboolean has_state;
  BroUnitState state;
  int limit;
  int offset;
} BroUnitQuery;

/* Everything zero. */
BroUnitQuery *bro_unit_query_new (void);
void bro_unit_query_free (BroUnitQuery *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroUnitQuery, bro_unit_query_free)

G_END_DECLS
