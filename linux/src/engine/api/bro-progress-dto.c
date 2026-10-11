/* bro-progress-dto.c */
#include "bro-progress-dto.h"

#include <math.h>

BroJsonValue *
_bro_dto_number (double d)
{
  return d == floor (d) && fabs (d) < 9e15 ? bro_json_value_new_integer ((gint64) d) : bro_json_value_new_number (d);
}

BroProgressDto *
bro_progress_dto_new (BroStepKind step, int step_index, int step_count, double fraction, BroBroMessage *operation)
{
  BroProgressDto *p = g_new0 (BroProgressDto, 1);
  p->step = step;
  p->step_index = step_index;
  p->step_count = step_count;
  p->fraction = fraction;
  p->operation = operation;
  return p;
}

BroProgressDto *
bro_progress_dto_copy (const BroProgressDto *progress)
{
  BroProgressDto *p;
  if (!progress)
    return NULL;
  p = g_memdup2 (progress, sizeof *progress);
  p->operation = bro_bro_message_copy (progress->operation);
  p->current = g_strdup (progress->current);
  return p;
}

void
bro_progress_dto_free (BroProgressDto *progress)
{
  if (!progress)
    return;
  bro_bro_message_free (progress->operation);
  g_free (progress->current);
  g_free (progress);
}

BroJsonValue *
bro_progress_dto_json (const BroProgressDto *p)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "step", bro_json_value_new_string (bro_step_kind_to_wire (p->step)));
  bro_json_value_set (o, "stepIndex", bro_json_value_new_integer (p->step_index));
  bro_json_value_set (o, "stepCount", bro_json_value_new_integer (p->step_count));
  bro_json_value_set (o, "fraction", _bro_dto_number (p->fraction));
  if (p->has_current_fraction)
    bro_json_value_set (o, "currentFraction", _bro_dto_number (p->current_fraction));
  bro_json_value_set (o, "operation", bro_bro_message_to_json (p->operation));
  if (p->current)
    bro_json_value_set (o, "current", bro_json_value_new_string (p->current));
  if (p->has_seconds_left)
    bro_json_value_set (o, "secondsLeft", _bro_dto_number (p->seconds_left));
  return o;
}
