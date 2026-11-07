/* bro-verify-rips-step.c */
#include "bro-verify-rips-step.h"
#include "bro-acquire-output.h"
#include "bro-backup-check.h"
#include "bro-plan-output.h"
#include "bro-probe-output.h"
#include "bro-rip-check.h"
#include "bro-steps-private.h"

#include <math.h>
#include <string.h>

struct _BroVerifyRipsStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroVerifyRipsStep, bro_verify_rips_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_VERIFY_RIPS;
}

static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  return bro_resource_set_new ();
}

/* The report: {checked: [@passed]}; takes @message (NULL: done). */
static BroStepReport *
report (GPtrArray *passed, BroBroMessage *message)
{
  BroJsonValue *checked = bro_json_value_new_array ();
  BroStepReport *rep;
  for (guint i = 0; i < passed->len; i++)
    bro_json_value_append (checked, bro_json_value_new_string (passed->pdata[i]));
  rep = bro_step_report_new (_bro_param ("checked", checked));
  if (message)
    {
      rep->error = bro_bro_message_to_error (message, NULL);
      bro_bro_message_free (message);
    }
  return rep;
}

static const BroTitle *
title_at (const BroListing *listing, int index)
{
  for (guint i = 0; listing && i < listing->titles->len; i++)
    if (((BroTitle *) listing->titles->pdata[i])->index == index)
      return listing->titles->pdata[i];
  return NULL;
}

/* @title below 0: a file no title owns, whose reason is the problem itself. */
static BroJsonValue *
reason_json (int title, const BroBroMessage *problem)
{
  g_autofree char *number = g_strdup_printf ("%d", title);
  BroJsonValue *params;
  g_autoptr (BroBroMessage) m = NULL;
  if (title < 0)
    return bro_bro_message_to_json (problem);
  params = _bro_param ("title", bro_json_value_new_string (number));
  bro_json_value_set (params, "reason", bro_bro_message_to_json (problem));
  m = bro_bro_message_new (BRO_MSG_RIP_TITLE_REASON, params, BRO_SEVERITY_INFO);
  return bro_bro_message_to_json (m);
}

static BroBroMessage *
mismatch (const char *file, BroJsonValue *problems)
{
  BroJsonValue *params = _bro_param ("file", bro_json_value_new_string (file));
  bro_json_value_set (params, "problems", problems);
  return bro_bro_message_new (BRO_MSG_RIPCHECK_MISMATCH, params, BRO_SEVERITY_INFO);
}

/* One run over the files. */
typedef struct {
  const BroStepContext *context;
  const char *staging;
  GPtrArray *passed;
  GArray *failed_titles;     /* int; below 0: a file no title owns */
  GPtrArray *failed_problems;
  guint total;
  guint n;
} Check;

typedef enum { CHECK_GO_ON, CHECK_NO_MKVMERGE, CHECK_ERROR } CheckEnd;

/* @index below 0: a file no title owns. @title: its listing title, when there is one. */
static CheckEnd
check_file (Check *k, int index, const BroTitle *title, const char *file, BroBroError **error)
{
  const BroStepContext *context = k->context;
  g_autofree char *path = g_strconcat (k->staging, "/", file, NULL);
  g_autoptr (BroBroMessage) operation = bro_bro_message_new (BRO_MSG_RIPCHECK_CHECKING, _bro_param ("file", bro_json_value_new_string (file)),
                                                             BRO_SEVERITY_INFO);
  g_autoptr (BroBroError) failure_error = NULL;
  g_autoptr (BroMkvProbe) probe = NULL;
  BroBroMessage *failure = NULL;
  bro_progress_reporter_report (context->progress, (double) k->n++ / k->total, operation, NULL);
  probe = bro_matroska_probe (context->tools->matroska, path, context->cancel, &failure_error);
  if (failure_error)
    {
      if (strcmp (failure_error->code, "tool.missing") == 0 || strcmp (failure_error->code, "tool.notFoundAt") == 0)
        {
          _bro_step_log (context, BRO_MSG_RIPCHECK_CANNOT_CHECK, NULL, BRO_SEVERITY_WARNING);
          return CHECK_NO_MKVMERGE;
        }
      *error = g_steal_pointer (&failure_error);
      return CHECK_ERROR;
    }
  if (!probe)
    failure = bro_bro_message_new (BRO_MSG_RIPCHECK_UNREADABLE, _bro_param ("file", bro_json_value_new_string (file)), BRO_SEVERITY_INFO);
  else if (title)
    {
      g_autoptr (BroRipCheckResult) result = bro_rip_check_check (probe, title);
      for (guint i = 0; i < result->notes->len; i++)
        {
          BroJsonValue *params = _bro_param ("file", bro_json_value_new_string (file));
          bro_json_value_set (params, "note", bro_bro_message_to_json (result->notes->pdata[i]));
          _bro_step_log (context, BRO_MSG_RIPCHECK_NOTE, params, BRO_SEVERITY_WARNING);
        }
      if (result->problems->len > 0)
        {
          BroJsonValue *problems = bro_json_value_new_array ();
          for (guint i = 0; i < result->problems->len; i++)
            bro_json_value_append (problems, bro_bro_message_to_json (result->problems->pdata[i]));
          failure = mismatch (file, problems);
        }
    }
  else if (index < 0 && probe->tracks->len == 0)
    {
      BroJsonValue *problems = bro_json_value_new_array ();
      g_autoptr (BroBroMessage) none = bro_bro_message_new (BRO_MSG_RIPCHECK_NO_TRACKS, NULL, BRO_SEVERITY_INFO);
      bro_json_value_append (problems, bro_bro_message_to_json (none));
      failure = mismatch (file, problems);
    }
  if (failure)
    {
      BroJsonValue *params = index < 0 ? bro_json_value_new_object () : _bro_param ("title", bro_json_value_new_integer (index));
      bro_json_value_set (params, "problem", bro_bro_message_to_json (failure));
      _bro_step_log (context, index < 0 ? BRO_MSG_RIP_OTHER_CHECK_FAILED : BRO_MSG_RIP_CHECK_FAILED, params, BRO_SEVERITY_ERROR);
      g_array_append_val (k->failed_titles, index);
      g_ptr_array_add (k->failed_problems, failure);
      return CHECK_GO_ON;
    }
  {
    BroJsonValue *params = _bro_param ("file", bro_json_value_new_string (file));
    bro_json_value_set (params, "duration", bro_json_value_new_integer ((gint64) round (probe->has_duration ? probe->duration_seconds : 0)));
    bro_json_value_set (params, "tracks", bro_json_value_new_integer (probe->tracks->len));
    bro_json_value_set (params, "chapters", bro_json_value_new_integer (probe->chapter_count));
    _bro_step_log (context, BRO_MSG_RIPCHECK_CHECKED, params, BRO_SEVERITY_INFO);
  }
  if (title && title->tracks->len > 0 && probe->tracks->len < title->tracks->len)
    {
      BroJsonValue *params = _bro_param ("file", bro_json_value_new_string (file));
      bro_json_value_set (params, "have", bro_json_value_new_integer (probe->tracks->len));
      bro_json_value_set (params, "total", bro_json_value_new_integer (title->tracks->len));
      _bro_step_log (context, BRO_MSG_RIPCHECK_TRACK_SUBSET, params, BRO_SEVERITY_INFO);
    }
  g_ptr_array_add (k->passed, g_strdup (file));
  return CHECK_GO_ON;
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (context->job);
  g_autoptr (BroAcquireOutput) acquired = bro_acquire_output_of (context->job);
  g_autoptr (BroProbeOutput) probed = NULL;
  g_autoptr (GPtrArray) passed = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GArray) failed_titles = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GPtrArray) failed_problems = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  g_autofree char *staging = NULL;
  Check k = { context, NULL, passed, failed_titles, failed_problems, 0, 0 };
  if (!plan || !acquired)
    return _bro_step_skipped ();
  if (!(staging = bro_library_port_staging_directory (context->library, plan->library_id, &context->job->id, error)))
    return NULL;
  k.staging = staging;
  {
    BroJsonValue *probe = bro_job_snapshot_output (context->job, BRO_STEP_KIND_PROBE);
    g_autoptr (BroJsonValue) none = bro_json_value_new_null ();
    probed = bro_probe_output_decode (probe ? probe : none);
  }
  if (acquired->backup)
    {
      g_autofree char *path = g_strconcat (staging, "/", acquired->backup, NULL);
      g_autoptr (BroBroMessage) problem = bro_backup_check_problem (context->fs, path);
      if (problem)
        return report (passed, bro_bro_message_new (BRO_MSG_BACKUP_CHECK_FAILED, _bro_param ("problem", bro_bro_message_to_json (problem)),
                                                    BRO_SEVERITY_ERROR));
      g_ptr_array_add (passed, g_strdup (acquired->backup));
    }
  k.total = acquired->others->len;
  for (guint i = 0; i < acquired->titles->len; i++)
    k.total += ((BroAcquiredTitle *) acquired->titles->pdata[i])->files->len;
  for (guint i = 0; i <= acquired->titles->len; i++)
    {
      BroAcquiredTitle *t = i < acquired->titles->len ? acquired->titles->pdata[i] : NULL; /* NULL: the files no title owns, last */
      GPtrArray *files = t ? t->files : acquired->others;
      const BroTitle *title = t ? title_at (probed->listing, t->index) : NULL;
      for (guint j = 0; j < files->len; j++)
        switch (check_file (&k, t ? t->index : -1, title, files->pdata[j], error))
          {
          case CHECK_GO_ON:
            break;
          case CHECK_NO_MKVMERGE:
            return report (passed, NULL);
          case CHECK_ERROR:
            return NULL;
          }
    }
  if (failed_titles->len == 0)
    return report (passed, NULL);
  if (failed_titles->len == 1)
    return report (passed, bro_bro_message_new (BRO_MSG_RIP_FAILED, _bro_param ("reason", reason_json (g_array_index (failed_titles, int, 0),
                                                                                                        failed_problems->pdata[0])),
                                                BRO_SEVERITY_ERROR));
  {
    BroJsonValue *reasons = bro_json_value_new_array ();
    BroJsonValue *params = _bro_param ("count", bro_json_value_new_integer (failed_titles->len));
    for (guint i = 0; i < failed_titles->len; i++)
      bro_json_value_append (reasons, reason_json (g_array_index (failed_titles, int, i), failed_problems->pdata[i]));
    bro_json_value_set (params, "reasons", reasons);
    return report (passed, bro_bro_message_new (BRO_MSG_RIP_TITLES_FAILED, params, BRO_SEVERITY_ERROR));
  }
}

static void
step_iface_init (BroStepInterface *iface)
{
  iface->kind = step_kind;
  iface->resumability = step_resumability;
  iface->resources = step_resources;
  iface->run = step_run;
}

static void
bro_verify_rips_step_class_init (BroVerifyRipsStepClass *klass)
{
}

static void
bro_verify_rips_step_init (BroVerifyRipsStep *self)
{
}

BroVerifyRipsStep *
bro_verify_rips_step_new (void)
{
  return g_object_new (BRO_TYPE_VERIFY_RIPS_STEP, NULL);
}
