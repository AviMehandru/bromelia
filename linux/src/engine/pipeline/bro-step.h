/* bro-step.h: BroStep: one step of a job (plan §12.1). run is called on a worker thread, never on the executor: it may
 * block, checks its context's cancellation token and reports through the context. An expected failure is returned
 * (NULL and @error set), or reported in BroStepReport.error (with what the outcome needs). */
#pragma once

#include "bro-job-snapshot.h"
#include "bro-resource-set.h"
#include "bro-resumability.h"
#include "bro-step-context.h"
#include "bro-step-report.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_STEP (bro_step_get_type ())
G_DECLARE_INTERFACE (BroStep, bro_step, BRO, STEP, GObject)

struct _BroStepInterface {
  GTypeInterface parent_iface;
  BroStepKind (*kind) (BroStep *self);
  BroResumability (*resumability) (BroStep *self);
  const char *(*resume_checkpoint) (BroStep *self); /* optional */
  BroResourceSet *(*resources) (BroStep *self, const BroJobSnapshot *job);
  BroStepReport *(*run) (BroStep *self, const BroStepContext *context, BroBroError **error);
};

BroStepKind bro_step_kind (BroStep *self);

BroResumability bro_step_resumability (BroStep *self);

/* The kind of checkpoint (BroCheckpointer) a resumeFrom step resumes from by itself after a restart (plan §12.1's
 * resumeFrom(checkpointKind)); NULL: none, the step is interrupted. A step without the function has none. */
const char *bro_step_resume_checkpoint (BroStep *self);

/* What the step holds while it runs: a drive, the acquisition slot, library writes, cpu, io. Free it. */
BroResourceSet *bro_step_resources (BroStep *self, const BroJobSnapshot *job);

/* On a worker thread. */
BroStepReport *bro_step_run (BroStep *self, const BroStepContext *context, BroBroError **error);

G_END_DECLS
