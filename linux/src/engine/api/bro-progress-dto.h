/* bro-progress-dto.h: a running job's progress (dtos.json Progress): fraction of the whole job, current_fraction of
 * the current step. */
#pragma once

#include "bro-bro-message.h"
#include "bro-step-kind.h"

G_BEGIN_DECLS

typedef struct {
  BroStepKind step;
  int step_index;
  int step_count;
  double fraction;
  gboolean has_current_fraction;
  double current_fraction;
  BroBroMessage *operation;
  char *current; /* nullable */
  gboolean has_seconds_left;
  double seconds_left;
} BroProgressDto;

/* Takes ownership of @operation. */
BroProgressDto *bro_progress_dto_new (BroStepKind step, int step_index, int step_count, double fraction, BroBroMessage *operation);
BroProgressDto *bro_progress_dto_copy (const BroProgressDto *progress);
void bro_progress_dto_free (BroProgressDto *progress);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_progress_dto_json (const BroProgressDto *progress);

/* A whole number as an integer, so 1.0 is written 1 on every platform (the DTOs share it). */
BroJsonValue *_bro_dto_number (double d);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroProgressDto, bro_progress_dto_free)

G_END_DECLS
