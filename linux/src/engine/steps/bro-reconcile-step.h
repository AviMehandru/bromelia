/* bro-reconcile-step.h: BroReconcileStep: Step 3 (plan §12.4, §20.1), only when a session made the choices: the disc must still be the one that
 * was opened (disc.changed with why), and each ticked title must be found in the listing read now
 * (bro_listing_match_map_titles; hand-picked tracks must not have changed). Logs disc.titleNumbersChanged when MakeMKV
 * numbers them differently now. Skipped without a session, or when Probe had to use the opened listing. Data:
 * BroReconcileOutput. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_RECONCILE_STEP (bro_reconcile_step_get_type ())
G_DECLARE_FINAL_TYPE (BroReconcileStep, bro_reconcile_step, BRO, RECONCILE_STEP, GObject)

BroReconcileStep *bro_reconcile_step_new (void);

G_END_DECLS
