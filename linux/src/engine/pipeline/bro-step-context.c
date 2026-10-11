/* bro-step-context.c */
#include "bro-step-context.h"

static gpointer
ref_or_null (gpointer object)
{
  return object ? g_object_ref (object) : NULL;
}

BroStepContext *
bro_step_context_new (BroJobSnapshot *job, BroCancellationToken *cancel, BroProgressReporter *progress, BroJobLogger *log,
                      BroDecisionPort *decisions, const BroStepEnvironment *environment, const char *job_directory)
{
  BroStepContext *c = g_new0 (BroStepContext, 1);
  c->job = job;
  c->cancel = bro_cancellation_token_ref (cancel);
  c->progress = progress;
  c->log = log;
  c->decisions = decisions;
  c->tools = bro_tool_set_copy (environment->tools);
  c->fs = ref_or_null (environment->fs);
  c->clock = ref_or_null (environment->clock);
  c->drives = ref_or_null (environment->drives);
  c->library = ref_or_null (environment->library);
  c->catalog = ref_or_null (environment->catalog);
  c->job_directory = g_strdup (job_directory);
  return c;
}

void
bro_step_context_free (BroStepContext *context)
{
  if (!context)
    return;
  bro_job_snapshot_free (context->job);
  bro_cancellation_token_unref (context->cancel);
  g_clear_object (&context->progress);
  g_clear_object (&context->log);
  g_clear_object (&context->decisions);
  bro_tool_set_free (context->tools);
  g_clear_object (&context->fs);
  g_clear_object (&context->clock);
  g_clear_object (&context->drives);
  g_clear_object (&context->library);
  g_clear_object (&context->catalog);
  g_free (context->job_directory);
  g_free (context);
}
