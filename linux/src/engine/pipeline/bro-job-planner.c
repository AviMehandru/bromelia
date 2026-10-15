/* bro-job-planner.c */
#include "bro-job-planner.h"

#include "bro-job-outcome.h"

static void
add (GArray *steps, BroStepKind kind)
{
  g_array_append_val (steps, kind);
}

GArray *
bro_job_planner_initial (const BroJobRequest *request)
{
  GArray *steps = g_array_new (FALSE, FALSE, sizeof (BroStepKind));
  if (request->kind != BRO_JOB_KIND_VIDEO_DISC)
    return steps;
  if (request->automatic && request->drive_id)
    add (steps, BRO_STEP_KIND_AWAIT_MEDIA);
  add (steps, BRO_STEP_KIND_PROBE);
  if (request->session_id)
    add (steps, BRO_STEP_KIND_RECONCILE);
  add (steps, BRO_STEP_KIND_IDENTIFY);
  add (steps, BRO_STEP_KIND_PLAN);
  return steps;
}

static gint
by_seq (gconstpointer a, gconstpointer b)
{
  return (*(BroStepRecord *const *) a)->seq - (*(BroStepRecord *const *) b)->seq;
}

GPtrArray *
bro_job_planner_done (GPtrArray *steps)
{
  GPtrArray *done = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_record_free);
  g_autoptr (GPtrArray) sorted = g_ptr_array_new ();
  for (guint i = 0; i < steps->len; i++)
    g_ptr_array_add (sorted, steps->pdata[i]);
  g_ptr_array_sort (sorted, by_seq);
  for (guint i = 0; i < sorted->len; i++)
    {
      BroStepRecord *s = sorted->pdata[i];
      gboolean finished = s->state == BRO_STEP_STATE_SUCCEEDED || s->state == BRO_STEP_STATE_SKIPPED
                          || (s->state == BRO_STEP_STATE_FAILED && bro_job_outcome_after_commit (s->kind));
      if (s->seq != (int) done->len || !finished)
        break;
      g_ptr_array_add (done, bro_step_record_copy (s));
    }
  return done;
}
