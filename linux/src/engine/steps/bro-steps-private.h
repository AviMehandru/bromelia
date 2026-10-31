/* bro-steps-private.h: what the step files share: building params, reports and log lines. */
#pragma once

#include "bro-message-code.h"
#include "bro-step-context.h"
#include "bro-step-report.h"

G_BEGIN_DECLS

/* A chosen profile and library (each nullable) as the resolver's session layer: the profile's fields but id and name
 * (an id that isn't configured adds none), then the library. NULL with nothing chosen. Shared by Plan and
 * bro_step_sources_profile; in bro-step-sources.c. */
BroJsonValue *_bro_step_sources_layer (const BroConfig *config, const char *profile_id, const char *library_id);

/* {key: value}; takes @value. */
static inline BroJsonValue *
_bro_param (const char *key, BroJsonValue *value)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, key, value);
  return o;
}

static inline BroStepReport *
_bro_step_skipped (void)
{
  BroStepReport *r = bro_step_report_new (NULL);
  r->skipped = TRUE;
  return r;
}

/* A failed report; takes @error. */
static inline BroStepReport *
_bro_step_failed (BroBroError *error)
{
  BroStepReport *r = bro_step_report_new (NULL);
  r->error = error;
  return r;
}

/* Logs @code with @params (taken; NULL: none). */
static inline void
_bro_step_log (const BroStepContext *context, BroMessageCode code, BroJsonValue *params, BroSeverity severity)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, params, severity);
  bro_job_logger_log (context->log, m);
}

static inline void
_bro_step_progress (const BroStepContext *context, double fraction, BroMessageCode operation)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (operation, NULL, BRO_SEVERITY_INFO);
  bro_progress_reporter_report (context->progress, fraction, m, NULL);
}

G_END_DECLS
