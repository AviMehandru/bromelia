/* bro-selection.h: the titles the rules chose (in index order), the reason for every title, whether the user
 * must choose, and the error that stopped the rules (an invalid pattern). */
#pragma once

#include "bro-selection-trace.h"

G_BEGIN_DECLS

typedef struct {
  GArray *indices;   /* int */
  GPtrArray *trace;  /* BroSelectionTrace *, one per title of the listing */
  gboolean requires_manual_choice;
  BroBroMessage *error; /* nullable */
} BroSelection;

void bro_selection_free (BroSelection *selection);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSelection, bro_selection_free)

G_END_DECLS
