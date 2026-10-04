/* bro-step-repository.h: BroStepRepository: The job_steps table. */
#pragma once

#include "bro-bro-error.h"
#include "bro-id.h"
#include "bro-step-record.h"
#include "bro-step-result.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_STEP_REPOSITORY (bro_step_repository_get_type ())
G_DECLARE_INTERFACE (BroStepRepository, bro_step_repository, BRO, STEP_REPOSITORY, GObject)

struct _BroStepRepositoryInterface {
  GTypeInterface parent_iface;

  gboolean (*save) (BroStepRepository *self, BroId job_id, int seq, const BroStepResult *result, BroBroError **error);
  GPtrArray *(*completed) (BroStepRepository *self, BroId job_id, BroBroError **error);
  GPtrArray *(*load) (BroStepRepository *self, BroId job_id, BroBroError **error);
};

/* Records how step seq ended. FALSE and @error set on failure. */
gboolean bro_step_repository_save (BroStepRepository *self, BroId job_id, int seq, const BroStepResult *result, BroBroError **error);

/* The steps that succeeded, in order. */
GPtrArray *bro_step_repository_completed (BroStepRepository *self, BroId job_id, BroBroError **error);

/* Every step, in order. */
GPtrArray *bro_step_repository_load (BroStepRepository *self, BroId job_id, BroBroError **error);

G_END_DECLS
