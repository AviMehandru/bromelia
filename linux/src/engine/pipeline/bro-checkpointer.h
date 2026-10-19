/* bro-checkpointer.h: BroCheckpointer: what a resumable step keeps between its attempts in one job (plan §12.2), stored with the step's row (job_steps.checkpoint), implemented by the runner. A checkpoint is JSON with a "kind": RecoveryService resumes a resumeFrom step after a restart only from a checkpoint of the kind the step names (bro_step_resume_checkpoint). */
#pragma once

#include "bro-bro-error.h"
#include "bro-json-value.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_CHECKPOINTER (bro_checkpointer_get_type ())
G_DECLARE_INTERFACE (BroCheckpointer, bro_checkpointer, BRO, CHECKPOINTER, GObject)

struct _BroCheckpointerInterface {
  GTypeInterface parent_iface;
  BroJsonValue *(*saved) (BroCheckpointer *self);
  gboolean (*save) (BroCheckpointer *self, const BroJsonValue *checkpoint, BroBroError **error);
};

/* What an earlier attempt of the step saved in this job (before a restart, or a retry), a new reference; NULL when none. */
BroJsonValue *bro_checkpointer_saved (BroCheckpointer *self);

/* Stores @checkpoint with the step's row; returns once it is stored. Any thread. */
gboolean bro_checkpointer_save (BroCheckpointer *self, const BroJsonValue *checkpoint, BroBroError **error);

G_END_DECLS
