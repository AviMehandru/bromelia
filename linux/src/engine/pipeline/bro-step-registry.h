/* bro-step-registry.h: BroStepRegistry: the steps by kind. The composition root registers the standard steps; tests
 * register their own. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_STEP_REGISTRY (bro_step_registry_get_type ())
G_DECLARE_FINAL_TYPE (BroStepRegistry, bro_step_registry, BRO, STEP_REGISTRY, GObject)

/* @steps: BroStep *; takes references. */
BroStepRegistry *bro_step_registry_new (GPtrArray *steps);

/* The phase 3 steps (AwaitMedia, Probe, Reconcile, Identify, Plan, Decide, Acquire, Release, VerifyRips, Name, Seal,
 * Commit, Notify, KeepApart); both composition roots use it, so their lists can't drift. */
BroStepRegistry *bro_step_registry_standard (void);

/* The step of @kind (borrowed), or NULL. */
BroStep *bro_step_registry_step (BroStepRegistry *self, BroStepKind kind);

G_END_DECLS
