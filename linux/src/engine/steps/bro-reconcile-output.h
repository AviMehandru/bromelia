/* bro-reconcile-output.h: what ReconcileStep found: each chosen title of the opened listing → its number in the
 * listing read now. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  GHashTable *title_map; /* GINT_TO_POINTER (opened) → GINT_TO_POINTER (now) */
} BroReconcileOutput;

/* Takes ownership of @title_map (NULL: empty). */
BroReconcileOutput *bro_reconcile_output_new (GHashTable *title_map);
void bro_reconcile_output_free (BroReconcileOutput *output);

/* {"titleMap": [[opened, now], …]}, by the opened number. */
BroJsonValue *bro_reconcile_output_encode (const BroReconcileOutput *output);
BroReconcileOutput *bro_reconcile_output_decode (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroReconcileOutput, bro_reconcile_output_free)

G_END_DECLS
