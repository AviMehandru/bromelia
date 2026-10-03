/* bro-step-filter.c */
#include "bro-step-filter.h"

gboolean
bro_step_filter_applies (const BroStepDefinition *step, BroOutcome outcome)
{
  if (!step->enabled || outcome == BRO_OUTCOME_SKIPPED)
    return FALSE;
  gboolean success = outcome == BRO_OUTCOME_SUCCEEDED || outcome == BRO_OUTCOME_SUCCEEDED_STEPS_FAILED;
  switch (step->run_on) {
  case BRO_RUN_ON_SUCCESS:
    return success;
  case BRO_RUN_ON_FAILURE:
    return !success;
  case BRO_RUN_ON_ALWAYS:
    return TRUE;
  }
  return FALSE;
}

BroStatusWord
bro_step_filter_status_word (BroOutcome outcome)
{
  switch (outcome) {
  case BRO_OUTCOME_SUCCEEDED:
  case BRO_OUTCOME_SUCCEEDED_STEPS_FAILED:
    return BRO_STATUS_WORD_SUCCESS;
  case BRO_OUTCOME_SUCCEEDED_WITH_READ_ERRORS:
    return BRO_STATUS_WORD_ERRORS;
  case BRO_OUTCOME_CANCELLED:
    return BRO_STATUS_WORD_CANCELLED;
  case BRO_OUTCOME_SKIPPED:
    return BRO_STATUS_WORD_SKIPPED;
  case BRO_OUTCOME_FAILED:
  case BRO_OUTCOME_INTERRUPTED:
    return BRO_STATUS_WORD_FAILED;
  }
  return BRO_STATUS_WORD_FAILED;
}
