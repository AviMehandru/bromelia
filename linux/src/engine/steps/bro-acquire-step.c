/* bro-acquire-step.c */
#include "bro-acquire-step.h"
#include "bro-acquire-output.h"
#include "bro-backup-check.h"
#include "bro-conflict-namer.h"
#include "bro-directory-entry.h"
#include "bro-file-info.h"
#include "bro-file-tree.h"
#include "bro-listing-match.h"
#include "bro-makemkv-sink.h"
#include "bro-one-pass.h"
#include "bro-plan-output.h"
#include "bro-probe-output.h"
#include "bro-source-resolver.h"
#include "bro-space-estimate.h"
#include "bro-step-sources.h"
#include "bro-steps-private.h"
#include "bro-volume-info.h"

#include <string.h>

struct _BroAcquireStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroAcquireStep, bro_acquire_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

/* ---- a run's progress as a part of the step's ---- */

#define TYPE_PORTION (portion_get_type ())
G_DECLARE_FINAL_TYPE (Portion, portion, ACQUIRE, PORTION, GObject)

struct _Portion {
  GObject parent_instance;
  BroProgressReporter *inner;
  double from;
  double width;
};

static void portion_progress_init (BroProgressReporterInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (Portion, portion, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_PROGRESS_REPORTER, portion_progress_init))

static void
portion_report (BroProgressReporter *self, double fraction, const BroBroMessage *operation, const char *current)
{
  Portion *p = (Portion *) self;
  bro_progress_reporter_report (p->inner, p->from + p->width * CLAMP (fraction, 0, 1), operation, current);
}

static void
portion_progress_init (BroProgressReporterInterface *iface)
{
  iface->report = portion_report;
}

static void
portion_finalize (GObject *object)
{
  g_clear_object (&((Portion *) object)->inner);
  G_OBJECT_CLASS (portion_parent_class)->finalize (object);
}

static void
portion_class_init (PortionClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = portion_finalize;
}

static void
portion_init (Portion *self)
{
}

/* ---- the step ---- */

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_ACQUIRE;
}

static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_RESUME_FROM;
}

static const char *
step_resume_checkpoint (BroStep *step)
{
  return BRO_ACQUIRE_STEP_BACKUP_CHECKPOINT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  BroResourceSet *set = bro_resource_set_new ();
  g_autoptr (BroDiscRequest) request = bro_disc_request_decode (job->request);
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  const char *drives[] = { request && request->source->kind == BRO_DISC_SOURCE_DRIVE ? request->source->value : NULL, NULL };
  const char *writes[] = { plan && plan->library_id && *plan->library_id ? plan->library_id : NULL, NULL };
  set->drives = g_strdupv ((char **) drives);
  set->acquisition = TRUE;
  set->library_writes = g_strdupv ((char **) writes);
  return set;
}

/* One run of the step. */
typedef struct {
  const BroStepContext *c;
  BroPlanOutput *plan;
  BroProbeOutput *probe;
  BroDiscRequest *request;
  char *staging;
  GPtrArray *titles;      /* BroAcquiredTitle * */
  GPtrArray *others;      /* char * */
  GPtrArray *read_errors; /* char * */
  char *backup;
  char *disc_info;
  BroMakemkvNotice *notice;
} Run;

static void
run_clear (Run *r)
{
  bro_plan_output_free (r->plan);
  bro_probe_output_free (r->probe);
  bro_disc_request_free (r->request);
  g_free (r->staging);
  g_ptr_array_unref (r->titles);
  g_ptr_array_unref (r->others);
  g_ptr_array_unref (r->read_errors);
  g_free (r->backup);
  g_free (r->disc_info);
  bro_makemkv_notice_free (r->notice);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (Run, run_clear)

static char *
in_staging (const Run *r, const char *name)
{
  return g_strconcat (r->staging, "/", name, NULL);
}

static gboolean
planned (const Run *r, int index)
{
  for (guint i = 0; i < r->plan->titles->len; i++)
    if (((BroTitleChoice *) r->plan->titles->pdata[i])->index == index)
      return TRUE;
  return FALSE;
}

static gboolean
done_title (const Run *r, int index)
{
  for (guint i = 0; i < r->titles->len; i++)
    if (((BroAcquiredTitle *) r->titles->pdata[i])->index == index)
      return TRUE;
  return FALSE;
}

static const BroTitle *
title_at (const BroListing *listing, int index)
{
  for (guint i = 0; i < listing->titles->len; i++)
    if (((BroTitle *) listing->titles->pdata[i])->index == index)
      return listing->titles->pdata[i];
  return NULL;
}

static const BroJsonValue *
profile_value (const Run *r, const char *section, const char *key)
{
  return bro_json_value_member (bro_json_value_member (r->plan->profile, section), key);
}

static int
plan_position (const Run *r, int index)
{
  for (guint i = 0; i < r->plan->titles->len; i++)
    if (((BroTitleChoice *) r->plan->titles->pdata[i])->index == index)
      return i;
  return G_MAXINT;
}

static int
by_plan (gconstpointer a, gconstpointer b, gpointer data)
{
  return plan_position (data, (*(BroAcquiredTitle *const *) a)->index) - plan_position (data, (*(BroAcquiredTitle *const *) b)->index);
}

static int
by_entry_name (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDirectoryEntry *const *) a)->name, (*(BroDirectoryEntry *const *) b)->name);
}

static gboolean
has_string (GPtrArray *list, const char *s)
{
  for (guint i = 0; i < list->len; i++)
    if (strcmp (list->pdata[i], s) == 0)
      return TRUE;
  return FALSE;
}

/* The report the step ends with: the output, its read errors, the notice when it failed; takes @error. */
static BroStepReport *
report (Run *r, BroBroError *error)
{
  g_autoptr (BroAcquireOutput) output = bro_acquire_output_new ();
  BroStepReport *rep;
  for (guint i = 0; i < r->titles->len; i++)
    g_ptr_array_add (output->titles, bro_acquired_title_copy (r->titles->pdata[i]));
  g_ptr_array_sort_with_data (output->titles, by_plan, r);
  for (guint i = 0; i < r->others->len; i++)
    g_ptr_array_add (output->others, g_strdup (r->others->pdata[i]));
  output->backup = g_strdup (r->backup);
  output->disc_info = g_strdup (r->disc_info);
  for (guint i = 0; i < r->read_errors->len; i++)
    g_ptr_array_add (output->read_errors, g_strdup (r->read_errors->pdata[i]));
  rep = bro_step_report_new (bro_acquire_output_encode (output));
  rep->read_errors = bro_acquire_output_read_error_count (output);
  rep->error = error;
  if (error && r->notice)
    rep->notice = bro_makemkv_notice_copy (r->notice);
  return rep;
}

/* Takes @message. */
static BroStepReport *
failed (Run *r, BroBroMessage *message)
{
  BroStepReport *rep = report (r, bro_bro_message_to_error (message, NULL));
  bro_bro_message_free (message);
  return rep;
}

static BroStepReport *
cancelled (Run *r)
{
  return report (r, bro_bro_error_new (bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL, NULL));
}

static BroBroError *
no_information (void)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (BRO_MSG_DISC_NO_INFORMATION, NULL, BRO_SEVERITY_ERROR);
  return bro_bro_message_to_error (m, NULL);
}

/* What a failed run said: its error, else its exit status. */
static BroBroMessage *
reason_of (const BroRunOutcome *outcome)
{
  if (outcome->error)
    return bro_bro_message_copy (outcome->error);
  return bro_bro_message_new (BRO_MSG_PROCESS_EXIT_STATUS, _bro_param ("status", bro_json_value_new_integer (outcome->exit_code)), BRO_SEVERITY_ERROR);
}

/* The checkpoint: the disc, the titles done, the backup a backup-then-MKV job rips from and the read errors of runs no
 * title owns (a backup's: its MKV files inherit them). The staging folder is synced first, so what the checkpoint names
 * is there after a power loss. */
static gboolean
save (Run *r, const char *kind, BroBroError **error)
{
  g_autoptr (BroJsonValue) checkpoint = bro_json_value_new_object ();
  BroJsonValue *titles = bro_json_value_new_array ();
  BroJsonValue *errors = bro_json_value_new_array ();
  if (!bro_file_system_sync_directory (r->c->fs, r->staging, error))
    {
      bro_json_value_unref (titles);
      bro_json_value_unref (errors);
      return FALSE;
    }
  for (guint i = 0; i < r->read_errors->len; i++)
    bro_json_value_append (errors, bro_json_value_new_string (r->read_errors->pdata[i]));
  for (guint i = 0; i < r->titles->len; i++)
    bro_json_value_append (titles, bro_acquired_title_encode (r->titles->pdata[i]));
  bro_json_value_set (checkpoint, "kind", bro_json_value_new_string (kind));
  bro_json_value_set (checkpoint, "fingerprint", r->probe->fingerprint ? bro_json_value_new_string (r->probe->fingerprint) : bro_json_value_new_null ());
  bro_json_value_set (checkpoint, "titles", titles);
  bro_json_value_set (checkpoint, "backup", r->backup ? bro_json_value_new_string (r->backup) : bro_json_value_new_null ());
  bro_json_value_set (checkpoint, "readErrors", errors);
  return bro_checkpointer_save (r->c->checkpoint, checkpoint, error);
}

/* The plan's profile, with Plan's one-pass minimum length when @min_length >= 0. */
static BroMakemkvInvocation *
invocation (Run *r, int min_length, const char *selection, BroBroError **error)
{
  BroJsonValue *profile = r->plan->profile;
  g_autoptr (BroJsonValue) changed = NULL;
  g_autoptr (BroEffectiveProfile) effective = NULL;
  if (min_length >= 0 && profile && profile->kind == BRO_JSON_VALUE_OBJECT)
    {
      BroJsonValue *old = bro_json_value_member (profile, "makemkv");
      BroJsonValue *makemkv = bro_json_value_new_object ();
      changed = bro_json_value_new_object ();
      for (guint i = 0; old && old->kind == BRO_JSON_VALUE_OBJECT && i < old->keys->len; i++)
        if (strcmp (old->keys->pdata[i], "minLengthSeconds") != 0)
          bro_json_value_set (makemkv, old->keys->pdata[i], bro_json_value_ref (old->items->pdata[i]));
      bro_json_value_set (makemkv, "minLengthSeconds", bro_json_value_new_integer (min_length));
      for (guint i = 0; i < profile->keys->len; i++)
        if (strcmp (profile->keys->pdata[i], "makemkv") != 0)
          bro_json_value_set (changed, profile->keys->pdata[i], bro_json_value_ref (profile->items->pdata[i]));
      bro_json_value_set (changed, "makemkv", makemkv);
      profile = changed;
    }
  effective = bro_effective_profile_new (bro_json_value_ref (profile), g_new0 (char *, 1),
                                         g_ptr_array_new_with_free_func ((GDestroyNotify) bro_resolve_trace_free));
  return bro_makemkv_setup_invocation (r->c->makemkv_setup, r->c->config, effective, selection, r->c->job_directory, error);
}

static BroMakemkvSink *
sink (Run *r, const BroBroMessage *operation, double from, double width)
{
  g_autoptr (Portion) p = g_object_new (TYPE_PORTION, NULL);
  p->inner = g_object_ref (r->c->progress);
  p->from = from;
  p->width = width;
  return bro_makemkv_sink_new (BRO_PROGRESS_REPORTER (p), r->c->log, operation);
}

static void
log_run (Run *r, const BroMakemkvRun *run)
{
  const BroBroMessage *problems[] = { run->isolation_warning, run->transcript_problem, run->settings_problem };
  for (guint i = 0; i < G_N_ELEMENTS (problems); i++)
    if (problems[i])
      {
        g_autoptr (BroBroMessage) warning = bro_bro_message_copy (problems[i]);
        warning->severity = BRO_SEVERITY_WARNING;
        bro_job_logger_log (r->c->log, warning);
      }
  if (run->outcome->abandoned)
    _bro_step_log (r->c, BRO_MSG_PROCESS_ABANDONED, _bro_param ("tool", bro_json_value_new_string ("makemkvcon")), BRO_SEVERITY_ERROR);
}

static void
progress (Run *r, double fraction, const BroBroMessage *operation)
{
  bro_progress_reporter_report (r->c->progress, fraction, operation, NULL);
}

static gboolean
is_cancelled (Run *r)
{
  return bro_cancellation_token_is_cancelled (r->c->cancel);
}

/* What a stopped run wrote: unreadable (P2-14), so removed; kept when the process may still be writing. */
static gboolean
remove_partial (Run *r, GPtrArray *names, gboolean abandoned, BroBroError **error)
{
  if (abandoned)
    return TRUE;
  for (guint i = 0; i < names->len; i++)
    {
      g_autofree char *path = in_staging (r, names->pdata[i]);
      if (!bro_file_tree_remove (r->c->fs, path, error))
        return FALSE;
      _bro_step_log (r->c, BRO_MSG_RIP_PARTIAL_REMOVED, _bro_param ("file", bro_json_value_new_string (names->pdata[i])), BRO_SEVERITY_INFO);
    }
  return TRUE;
}

static gboolean
write_disc_info (Run *r, const BroListing *listing, BroBroError **error)
{
  g_autofree char *path = in_staging (r, BRO_ACQUIRE_STEP_DISC_INFO_FILE);
  g_autoptr (BroJsonValue) json = bro_listing_encode (listing);
  g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (json);
  if (!bro_file_system_write_atomically (r->c->fs, path, bytes, 0644, error))
    return FALSE;
  g_free (r->disc_info);
  r->disc_info = g_strdup (BRO_ACQUIRE_STEP_DISC_INFO_FILE);
  _bro_step_log (r->c, BRO_MSG_RIP_DISC_INFO_WRITTEN, _bro_param ("file", bro_json_value_new_string (BRO_ACQUIRE_STEP_DISC_INFO_FILE)),
                 BRO_SEVERITY_INFO);
  return TRUE;
}

/* Renames a staging entry to a free name like @wanted; the new name. */
static char *
rename_free (Run *r, const char *name, const char *wanted, gboolean is_folder, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = bro_file_system_list (r->c->fs, r->staging, error);
  g_autoptr (GPtrArray) names = g_ptr_array_new ();
  g_autofree char *free_name = NULL;
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;
  if (!entries)
    return NULL;
  for (guint i = 0; i < entries->len; i++)
    g_ptr_array_add (names, ((BroDirectoryEntry *) entries->pdata[i])->name);
  g_ptr_array_add (names, NULL);
  free_name = bro_conflict_namer_next (wanted, (const char *const *) names->pdata, is_folder);
  from = in_staging (r, name);
  to = in_staging (r, free_name);
  if (!bro_file_system_rename (r->c->fs, from, to, error))
    return NULL;
  return g_steal_pointer (&free_name);
}

/* A backup into staging. TRUE with *@name set, or with *@stopped (the report the step ends with); FALSE and @error set
 * when it failed outright. */
static gboolean
make_backup (Run *r, gboolean decrypt, double from, double width, char **name, BroStepReport **stopped, BroBroError **error)
{
  gboolean iso = g_strcmp0 (bro_json_value_get_string (profile_value (r, "backup", "format"), NULL), "iso") == 0;
  g_autofree char *made = g_strdup (iso ? "backup.iso" : "backup");
  g_autofree char *destination = in_staging (r, made);
  g_autoptr (BroMakemkvSource) source = bro_step_sources_makemkv (r->request->source, r->c->drive_lookup, error);
  g_autoptr (BroBroMessage) operation = NULL;
  g_autoptr (BroMakemkvInvocation) inv = NULL;
  g_autoptr (BroMakemkvSink) s = NULL;
  g_autoptr (BroMakemkvRun) run = NULL;
  g_autoptr (BroFileInfo) info = NULL;
  g_autofree char *saved = NULL;
  const BroRunOutcome *outcome;
  if (!source)
    return FALSE;
  operation = bro_bro_message_new (BRO_MSG_BACKUP_BACKING_UP, _bro_param ("decrypted", bro_json_value_new_bool (decrypt)), BRO_SEVERITY_INFO);
  progress (r, from, operation);
  if (!(inv = invocation (r, -1, NULL, error)))
    return FALSE;
  s = sink (r, operation, from, width);
  if (!(run = bro_makemkv_backup (r->c->tools->makemkv, source, decrypt, destination, inv, BRO_RUN_SINK (s), r->c->cancel, error)))
    return FALSE;
  log_run (r, run);
  outcome = run->outcome;
  if (outcome->status == BRO_STATUS_WORD_CANCELLED || is_cancelled (r))
    {
      if (bro_file_system_exists (r->c->fs, destination))
        {
          g_autoptr (GPtrArray) names = g_ptr_array_new ();
          g_ptr_array_add (names, made);
          if (!remove_partial (r, names, outcome->abandoned, error))
            return FALSE;
        }
      *stopped = cancelled (r);
      return TRUE;
    }
  if (outcome->status == BRO_STATUS_WORD_FAILED)
    {
      g_autoptr (BroBroMessage) reason = reason_of (outcome);
      if (!r->notice && run->notice)
        r->notice = bro_makemkv_notice_copy (run->notice);
      if (bro_file_system_exists (r->c->fs, destination))
        g_ptr_array_add (r->others, g_strdup (made));
      *stopped = failed (r, bro_bro_message_new (BRO_MSG_BACKUP_FAILED, _bro_param ("reason", bro_bro_message_to_json (reason)), BRO_SEVERITY_ERROR));
      return TRUE;
    }
  if (!(info = bro_file_system_stat (r->c->fs, destination, error)))
    return FALSE;
  if (iso && info->is_directory)
    {
      char *renamed = rename_free (r, made, "backup", TRUE, error);
      if (!renamed)
        return FALSE;
      g_free (made);
      made = renamed;
      _bro_step_log (r->c, BRO_MSG_BACKUP_FOLDER_INSTEAD_OF_ISO, _bro_param ("folder", bro_json_value_new_string (made)), BRO_SEVERITY_WARNING);
    }
  else if (!iso && !info->is_directory)
    {
      char *renamed = rename_free (r, made, "backup.iso", FALSE, error);
      if (!renamed)
        return FALSE;
      g_free (made);
      made = renamed;
      _bro_step_log (r->c, BRO_MSG_BACKUP_IMAGE_FOR_FOLDER, _bro_param ("file", bro_json_value_new_string (made)), BRO_SEVERITY_INFO);
    }
  saved = in_staging (r, made);
  _bro_step_log (r->c, BRO_MSG_BACKUP_SAVED, _bro_param ("path", bro_json_value_new_string (saved)), BRO_SEVERITY_INFO);
  for (guint i = 0; i < outcome->read_errors->len; i++)
    g_ptr_array_add (r->read_errors, g_strdup (((BroRobotMessage *) outcome->read_errors->pdata[i])->text));
  *name = g_steal_pointer (&made);
  return TRUE;
}

/* rip.titleReason {title, reason} for a title's failure; the reason itself for an "all" run's (@title < 0). */
static BroJsonValue *
failure_json (int title, const BroBroMessage *reason)
{
  g_autoptr (BroBroMessage) wrapped = NULL;
  g_autofree char *number = NULL;
  BroJsonValue *params;
  if (title < 0)
    return bro_bro_message_to_json (reason);
  number = g_strdup_printf ("%d", title);
  params = _bro_param ("title", bro_json_value_new_string (number));
  bro_json_value_set (params, "reason", bro_bro_message_to_json (reason));
  wrapped = bro_bro_message_new (BRO_MSG_RIP_TITLE_REASON, params, BRO_SEVERITY_INFO);
  return bro_bro_message_to_json (wrapped);
}

/* Rips the plan's titles that aren't done yet from @source, whose listing is @listing (@map: the plan's index → its
 * index there). TRUE with *@stopped NULL when every run succeeded, or set to the report the step ends with; FALSE and
 * @error set when it failed outright. */
static gboolean
rip (Run *r, const BroMakemkvSource *source, const BroListing *listing, GHashTable *map, const char *checkpoint, double from, double width,
     BroStepReport **stopped, BroBroError **error)
{
  g_autoptr (GPtrArray) wanted = g_ptr_array_new (); /* BroTitleChoice *, the plan's */
  g_autoptr (GHashTable) run_map = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_autoptr (BroListingRun) pass_read = NULL;
  g_autoptr (GPtrArray) failure_titles = g_ptr_array_new ();                                       /* GINT_TO_POINTER (title + 1); 0: all */
  g_autoptr (GPtrArray) failure_reasons = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  const BroListing *run_listing = listing;
  gboolean hand_picked = FALSE;
  gboolean single;
  int min_length = -1;
  guint runs;
  for (guint i = 0; i < r->plan->titles->len; i++)
    {
      BroTitleChoice *t = r->plan->titles->pdata[i];
      if (done_title (r, t->index))
        continue;
      g_ptr_array_add (wanted, t);
      hand_picked |= t->tracks != NULL;
      g_hash_table_insert (run_map, GINT_TO_POINTER (t->index), g_hash_table_lookup (map, GINT_TO_POINTER (t->index)));
    }
  *stopped = NULL;
  if (wanted->len == 0)
    return TRUE;
  if (r->plan->has_one_pass_min_length && r->titles->len == 0 && !hand_picked)
    {
      int m = r->plan->one_pass_min_length;
      g_autoptr (BroBroMessage) operation = bro_bro_message_new (BRO_MSG_RIP_ONE_PASS_LISTING, NULL, BRO_SEVERITY_INFO);
      g_autoptr (BroMakemkvInvocation) inv = NULL;
      g_autoptr (BroMakemkvSink) s = NULL;
      g_autoptr (GArray) indices = g_array_new (FALSE, FALSE, sizeof (int));
      progress (r, from, operation);
      if (!(inv = invocation (r, m, NULL, error)))
        return FALSE;
      s = sink (r, operation, from, 0);
      if (!(pass_read = bro_makemkv_listing (r->c->tools->makemkv, source, inv, BRO_RUN_SINK (s), r->c->cancel, error)))
        return FALSE;
      log_run (r, pass_read->run);
      if (pass_read->run->outcome->status == BRO_STATUS_WORD_CANCELLED || is_cancelled (r))
        {
          *stopped = cancelled (r);
          return TRUE;
        }
      for (guint i = 0; i < wanted->len; i++)
        {
          int index = GPOINTER_TO_INT (g_hash_table_lookup (map, GINT_TO_POINTER (((BroTitleChoice *) wanted->pdata[i])->index)));
          g_array_append_val (indices, index);
        }
      if (bro_one_pass_matches (pass_read->listing, indices, listing))
        {
          g_autoptr (GHashTable) pass = bro_listing_match_map_titles (indices, listing, pass_read->listing, NULL, error);
          BroJsonValue *params;
          if (!pass)
            return FALSE;
          params = _bro_param ("count", bro_json_value_new_integer (indices->len));
          bro_json_value_set (params, "minLength", bro_json_value_new_integer (m));
          bro_json_value_set (params, "others", bro_json_value_new_integer ((gint64) listing->titles->len - indices->len));
          _bro_step_log (r->c, BRO_MSG_RIP_ONE_PASS, params, BRO_SEVERITY_INFO);
          for (guint i = 0; i < wanted->len; i++)
            {
              gpointer key = GINT_TO_POINTER (((BroTitleChoice *) wanted->pdata[i])->index);
              g_hash_table_insert (run_map, key, g_hash_table_lookup (pass, g_hash_table_lookup (map, key)));
            }
          run_listing = pass_read->listing;
          min_length = m;
        }
      else
        _bro_step_log (r->c, BRO_MSG_RIP_ONE_PASS_MISMATCH, _bro_param ("minLength", bro_json_value_new_integer (m)), BRO_SEVERITY_WARNING);
    }
  single = !hand_picked && wanted->len == run_listing->titles->len;
  for (guint i = 0; single && i < run_listing->titles->len; i++)
    {
      gboolean chosen = FALSE;
      for (guint j = 0; j < wanted->len; j++)
        chosen |= GPOINTER_TO_INT (g_hash_table_lookup (run_map, GINT_TO_POINTER (((BroTitleChoice *) wanted->pdata[j])->index)))
                  == ((BroTitle *) run_listing->titles->pdata[i])->index;
      single = chosen;
    }
  runs = single ? 1 : wanted->len;
  for (guint n = 0; n < runs; n++)
    {
      BroTitleChoice *first = wanted->pdata[n];
      g_autofree char *title = single ? g_strdup ("all")
                                      : g_strdup_printf ("%d", GPOINTER_TO_INT (g_hash_table_lookup (run_map, GINT_TO_POINTER (first->index))));
      g_autoptr (BroBroMessage) operation = NULL;
      g_autoptr (BroMakemkvInvocation) inv = NULL;
      g_autoptr (BroMakemkvSink) s = NULL;
      g_autoptr (BroMakemkvRun) run = NULL;
      const BroRunOutcome *outcome;
      double at = from + width * n / runs;
      if (is_cancelled (r))
        {
          *stopped = cancelled (r);
          return TRUE;
        }
      if (single)
        operation = bro_bro_message_new (BRO_MSG_RIP_RIPPING, _bro_param ("count", bro_json_value_new_integer (wanted->len)), BRO_SEVERITY_INFO);
      else
        {
          BroJsonValue *params = _bro_param ("title", bro_json_value_new_integer (first->index));
          bro_json_value_set (params, "n", bro_json_value_new_integer (n + 1));
          bro_json_value_set (params, "of", bro_json_value_new_integer (runs));
          operation = bro_bro_message_new (BRO_MSG_RIP_RIPPING_TITLE, params, BRO_SEVERITY_INFO);
        }
      progress (r, at, operation);
      if (!(inv = invocation (r, min_length, !single && first->tracks ? "+sel:all" : NULL, error)))
        return FALSE;
      s = sink (r, operation, at, width / runs);
      if (!(run = bro_makemkv_rip (r->c->tools->makemkv, source, title, r->staging, inv, BRO_RUN_SINK (s), r->c->cancel, error)))
        return FALSE;
      log_run (r, run);
      outcome = run->outcome;
      if (outcome->status == BRO_STATUS_WORD_CANCELLED || is_cancelled (r))
        {
          if (!remove_partial (r, outcome->produced, outcome->abandoned, error))
            return FALSE;
          *stopped = cancelled (r);
          return TRUE;
        }
      if (outcome->status == BRO_STATUS_WORD_FAILED)
        {
          BroBroMessage *reason = reason_of (outcome);
          g_autofree char *number = single ? g_strdup ("all") : g_strdup_printf ("%d", first->index);
          BroJsonValue *params = _bro_param ("title", bro_json_value_new_string (number));
          bro_json_value_set (params, "reason", bro_bro_message_to_json (reason));
          _bro_step_log (r->c, BRO_MSG_RIP_TITLE_FAILED, params, BRO_SEVERITY_ERROR);
          for (guint i = 0; i < outcome->produced->len; i++)
            {
              _bro_step_log (r->c, BRO_MSG_RIP_KEPT_UNDER_MAKEMKV_NAME, _bro_param ("file", bro_json_value_new_string (outcome->produced->pdata[i])),
                             BRO_SEVERITY_WARNING);
              g_ptr_array_add (r->others, g_strdup (outcome->produced->pdata[i]));
            }
          g_ptr_array_add (failure_titles, GINT_TO_POINTER (single ? 0 : first->index + 1));
          g_ptr_array_add (failure_reasons, reason);
          if (!r->notice && run->notice)
            r->notice = bro_makemkv_notice_copy (run->notice);
          continue;
        }
      {
        g_autoptr (GPtrArray) made = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_acquired_title_free);
        if (single)
          {
            for (guint j = 0; j < wanted->len; j++)
              {
                BroTitleChoice *t = wanted->pdata[j];
                const BroTitle *listed = title_at (run_listing, GPOINTER_TO_INT (g_hash_table_lookup (run_map, GINT_TO_POINTER (t->index))));
                const char *name = listed ? listed->output_file_name : "";
                BroAcquiredTitle *a = bro_acquired_title_new (t->index);
                for (guint k = 0; k < outcome->produced->len; k++)
                  if (*name && strcmp (outcome->produced->pdata[k], name) == 0)
                    g_ptr_array_add (a->files, g_strdup (name));
                for (guint k = 0; wanted->len == 1 && k < outcome->read_errors->len; k++)
                  g_ptr_array_add (a->read_errors, g_strdup (((BroRobotMessage *) outcome->read_errors->pdata[k])->text));
                g_ptr_array_add (made, a);
              }
            for (guint k = 0; k < outcome->produced->len; k++)
              {
                gboolean owned = FALSE;
                for (guint j = 0; j < made->len; j++)
                  owned |= has_string (((BroAcquiredTitle *) made->pdata[j])->files, outcome->produced->pdata[k]);
                if (!owned)
                  g_ptr_array_add (r->others, g_strdup (outcome->produced->pdata[k]));
              }
            for (guint k = 0; wanted->len > 1 && k < outcome->read_errors->len; k++)
              g_ptr_array_add (r->read_errors, g_strdup (((BroRobotMessage *) outcome->read_errors->pdata[k])->text));
          }
        else
          {
            BroAcquiredTitle *a = bro_acquired_title_new (first->index);
            for (guint k = 0; k < outcome->produced->len; k++)
              g_ptr_array_add (a->files, g_strdup (outcome->produced->pdata[k]));
            for (guint k = 0; k < outcome->read_errors->len; k++)
              g_ptr_array_add (a->read_errors, g_strdup (((BroRobotMessage *) outcome->read_errors->pdata[k])->text));
            g_ptr_array_add (made, a);
          }
        for (guint k = 0; k < outcome->produced->len; k++)
          {
            g_autofree char *path = in_staging (r, outcome->produced->pdata[k]);
            if (!bro_file_system_sync_file (r->c->fs, path, error))
              return FALSE;
          }
        for (guint j = 0; j < made->len; j++)
          g_ptr_array_add (r->titles, bro_acquired_title_copy (made->pdata[j]));
      }
      if (!save (r, checkpoint, error))
        return FALSE;
    }
  if (failure_titles->len == 0)
    return TRUE;
  if (failure_titles->len == 1)
    {
      BroJsonValue *reason = failure_json (GPOINTER_TO_INT (failure_titles->pdata[0]) - 1, failure_reasons->pdata[0]);
      *stopped = failed (r, bro_bro_message_new (BRO_MSG_RIP_FAILED, _bro_param ("reason", reason), BRO_SEVERITY_ERROR));
    }
  else
    {
      BroJsonValue *reasons = bro_json_value_new_array ();
      BroJsonValue *params = _bro_param ("count", bro_json_value_new_integer (failure_titles->len));
      for (guint i = 0; i < failure_titles->len; i++)
        bro_json_value_append (reasons, failure_json (GPOINTER_TO_INT (failure_titles->pdata[i]) - 1, failure_reasons->pdata[i]));
      bro_json_value_set (params, "reasons", reasons);
      *stopped = failed (r, bro_bro_message_new (BRO_MSG_RIP_TITLES_FAILED, params, BRO_SEVERITY_ERROR));
    }
  return TRUE;
}

static BroStepReport *
backup_then_mkv (Run *r, const BroListing *listing, const char *resumed, BroBroError **error)
{
  g_autofree char *name = g_strdup (resumed);
  g_autofree char *path = NULL;
  g_autoptr (BroFileInfo) info = NULL;
  g_autoptr (BroMakemkvSource) source = NULL;
  g_autoptr (BroBroMessage) reading = NULL;
  g_autoptr (BroMakemkvInvocation) inv = NULL;
  g_autoptr (BroMakemkvSink) s = NULL;
  g_autoptr (BroListingRun) read = NULL;
  g_autoptr (GArray) indices = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GHashTable) hand_picked = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_autoptr (GHashTable) map = NULL;
  BroStepReport *stopped = NULL;
  gboolean all_done = TRUE;
  double rips_from = 0;
  for (guint i = 0; i < r->plan->titles->len; i++)
    all_done &= done_title (r, ((BroTitleChoice *) r->plan->titles->pdata[i])->index);
  if (all_done)
    {
      g_free (r->backup);
      r->backup = g_strdup (resumed);
      return report (r, NULL);
    }
  if (!name)
    {
      g_autoptr (BroBroMessage) problem = NULL;
      g_autofree char *made = NULL;
      if (!make_backup (r, TRUE, 0, 0.5, &made, &stopped, error))
        return NULL;
      if (stopped)
        return stopped;
      name = g_steal_pointer (&made);
      path = in_staging (r, name);
      if ((problem = bro_backup_check_problem (r->c->fs, path)))
        return failed (r, bro_bro_message_new (BRO_MSG_BACKUP_CHECK_FAILED, _bro_param ("problem", bro_bro_message_to_json (problem)), BRO_SEVERITY_ERROR));
      if (!bro_file_tree_sync (r->c->fs, path, error))
        return NULL;
      g_free (r->backup);
      r->backup = g_strdup (name);
      if (!save (r, BRO_ACQUIRE_STEP_BACKUP_CHECKPOINT, error))
        return NULL;
      rips_from = 0.5;
    }
  g_free (r->backup);
  r->backup = g_strdup (name);
  if (!path)
    path = in_staging (r, name);
  if (!(info = bro_file_system_stat (r->c->fs, path, error)))
    return NULL;
  source = bro_source_resolver_source (path, info->is_directory);
  reading = bro_bro_message_new (BRO_MSG_DISC_OP_READING, NULL, BRO_SEVERITY_INFO);
  progress (r, rips_from, reading);
  if (!(inv = invocation (r, -1, NULL, error)))
    return NULL;
  s = sink (r, reading, rips_from, 0);
  if (!(read = bro_makemkv_listing (r->c->tools->makemkv, source, inv, BRO_RUN_SINK (s), r->c->cancel, error)))
    return NULL;
  log_run (r, read->run);
  if (read->run->outcome->status == BRO_STATUS_WORD_CANCELLED || is_cancelled (r))
    return cancelled (r);
  if (read->listing->titles->len == 0)
    {
      g_autoptr (BroBroMessage) reason = reason_of (read->run->outcome);
      BroJsonValue *params = _bro_param ("source", bro_json_value_new_string (path));
      bro_json_value_set (params, "reason", bro_bro_message_to_json (reason));
      return failed (r, bro_bro_message_new (BRO_MSG_DISC_NO_TITLES, params, BRO_SEVERITY_ERROR));
    }
  for (guint i = 0; i < r->plan->titles->len; i++)
    {
      BroTitleChoice *t = r->plan->titles->pdata[i];
      g_array_append_val (indices, t->index);
      if (t->tracks)
        g_hash_table_add (hand_picked, GINT_TO_POINTER (t->index));
    }
  if (!(map = bro_listing_match_map_titles (indices, listing, read->listing, hand_picked, error)))
    return NULL;
  if (bro_json_value_get_bool (profile_value (r, "archive", "writeDiscInfo"), FALSE) && !write_disc_info (r, read->listing, error))
    return NULL;
  if (!rip (r, source, read->listing, map, BRO_ACQUIRE_STEP_BACKUP_CHECKPOINT, rips_from, 1 - rips_from, &stopped, error))
    return NULL;
  if (stopped)
    return stopped;
  if (bro_json_value_get_bool (profile_value (r, "backup", "keepAfterMkv"), TRUE))
    return report (r, NULL);
  g_clear_pointer (&r->backup, g_free);
  if (!save (r, BRO_ACQUIRE_STEP_TITLES_CHECKPOINT, error)) /* the titles stand without the backup from here on */
    return NULL;
  _bro_step_log (r->c, BRO_MSG_BACKUP_REMOVED, _bro_param ("path", bro_json_value_new_string (path)), BRO_SEVERITY_INFO);
  if (!bro_file_tree_remove (r->c->fs, path, error))
    return NULL;
  return report (r, NULL);
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_auto (Run) r = { 0 };
  g_autoptr (BroLibraryHandle) library = NULL;
  g_autoptr (BroJsonValue) saved = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  g_autoptr (GHashTable) keep = g_hash_table_new (g_str_hash, g_str_equal);
  g_autofree char *resume_backup = NULL;
  const char *kind;
  gboolean same;
  r.c = context;
  r.titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_acquired_title_free);
  r.others = g_ptr_array_new_with_free_func (g_free);
  r.read_errors = g_ptr_array_new_with_free_func (g_free);
  if (!(r.plan = bro_plan_output_of (context->job)))
    {
      BroJsonValue *params = _bro_param ("detail", bro_json_value_new_string ("acquire without a plan"));
      *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_INTERNAL_UNEXPECTED), params, NULL);
      return NULL;
    }
  {
    BroJsonValue *probe = bro_job_snapshot_output (context->job, BRO_STEP_KIND_PROBE);
    g_autoptr (BroJsonValue) none = bro_json_value_new_null ();
    r.probe = bro_probe_output_decode (probe ? probe : none);
  }
  if (!(r.request = bro_step_sources_request (context->job, error)))
    return NULL;
  if (!(library = bro_library_port_require (context->library, r.plan->library_id, error)))
    return NULL;
  if (!(r.staging = bro_library_port_staging_directory (context->library, r.plan->library_id, &context->job->id, error)))
    return NULL;

  /* What an earlier attempt finished, with this disc. */
  saved = bro_checkpointer_saved (context->checkpoint);
  same = saved && saved->kind == BRO_JSON_VALUE_OBJECT && r.probe->fingerprint
         && g_strcmp0 (bro_json_value_get_string (bro_json_value_member (saved, "fingerprint"), NULL), r.probe->fingerprint) == 0;
  kind = saved ? bro_json_value_get_string (bro_json_value_member (saved, "kind"), NULL) : NULL;
  if (same && r.plan->mode == BRO_RIP_MODE_BACKUP_THEN_MKV && g_strcmp0 (kind, BRO_ACQUIRE_STEP_BACKUP_CHECKPOINT) == 0)
    {
      const char *b = bro_json_value_get_string (bro_json_value_member (saved, "backup"), NULL);
      g_autofree char *path = b && *b ? in_staging (&r, b) : NULL;
      if (path && bro_file_system_exists (context->fs, path))
        resume_backup = g_strdup (b);
    }
  if (same && ((r.plan->mode == BRO_RIP_MODE_MKV && g_strcmp0 (kind, BRO_ACQUIRE_STEP_TITLES_CHECKPOINT) == 0)
               || (r.plan->mode == BRO_RIP_MODE_BACKUP_THEN_MKV
                   && (g_strcmp0 (kind, BRO_ACQUIRE_STEP_TITLES_CHECKPOINT) == 0 || g_strcmp0 (kind, BRO_ACQUIRE_STEP_BACKUP_CHECKPOINT) == 0))))
    {
      BroJsonValue *titles = bro_json_value_member (saved, "titles");
      for (guint i = 0; i < bro_json_value_length (titles); i++)
        {
          g_autoptr (BroAcquiredTitle) t = bro_acquired_title_decode (bro_json_value_at (titles, i));
          gboolean there = t->files->len > 0;
          for (guint j = 0; there && j < t->files->len; j++)
            {
              g_autofree char *path = in_staging (&r, t->files->pdata[j]);
              there = bro_file_system_exists (context->fs, path);
            }
          if (there && planned (&r, t->index))
            g_ptr_array_add (r.titles, g_steal_pointer (&t));
        }
      BroJsonValue *errors = bro_json_value_member (saved, "readErrors"); /* a backup's */
      for (guint i = 0; i < bro_json_value_length (errors); i++)
        g_ptr_array_add (r.read_errors, g_strdup (bro_json_value_get_string (bro_json_value_at (errors, i), "")));
    }
  for (guint i = 0; i < r.titles->len; i++)
    {
      BroAcquiredTitle *t = r.titles->pdata[i];
      for (guint j = 0; j < t->files->len; j++)
        g_hash_table_add (keep, t->files->pdata[j]);
    }
  if (resume_backup)
    g_hash_table_add (keep, resume_backup);
  if (!(entries = bro_file_system_list (context->fs, r.staging, error)))
    return NULL;
  g_ptr_array_sort (entries, by_entry_name);
  for (guint i = 0; i < entries->len; i++)
    {
      const char *name = ((BroDirectoryEntry *) entries->pdata[i])->name;
      g_autofree char *path = NULL;
      if (g_hash_table_contains (keep, name))
        continue;
      path = in_staging (&r, name);
      if (!bro_file_tree_remove (context->fs, path, error))
        return NULL;
      _bro_step_log (context, BRO_MSG_RIP_LEFTOVER_REMOVED, _bro_param ("file", bro_json_value_new_string (name)), BRO_SEVERITY_INFO);
    }
  if (resume_backup)
    _bro_step_log (context, BRO_MSG_RECOVERY_RESUMED_FROM_BACKUP, NULL, BRO_SEVERITY_INFO);
  if (r.titles->len > 0)
    _bro_step_log (context, BRO_MSG_RIP_CONTINUED, _bro_param ("count", bro_json_value_new_integer (r.titles->len)), BRO_SEVERITY_INFO);

  if (r.plan->has_space_need)
    {
      gint64 kept = 0;
      for (guint i = 0; i < r.titles->len; i++)
        {
          const BroTitle *t = r.probe->listing ? title_at (r.probe->listing, ((BroAcquiredTitle *) r.titles->pdata[i])->index) : NULL;
          kept += t ? t->size_bytes : 0;
        }
      if (r.plan->space_need - kept > 0)
        {
          g_autoptr (BroVolumeInfo) volume = bro_file_system_volume (context->fs, library->path, error);
          BroBytes bytes = { r.plan->space_need - kept };
          if (!volume
              || !bro_space_reservations_reserve (context->space, volume->id, library->path, bro_space_estimate_required (bytes).count,
                                                  volume->free_bytes, error))
            return NULL;
        }
    }

  switch (r.plan->mode)
    {
    case BRO_RIP_MODE_INFO_ONLY:
      if (!r.probe->listing)
        {
          *error = no_information ();
          return NULL;
        }
      if (!write_disc_info (&r, r.probe->listing, error))
        return NULL;
      return report (&r, NULL);
    case BRO_RIP_MODE_BACKUP:
    case BRO_RIP_MODE_BACKUP_DECRYPTED:
      {
        BroStepReport *stopped = NULL;
        char *made = NULL;
        if (!make_backup (&r, r.plan->mode == BRO_RIP_MODE_BACKUP_DECRYPTED, 0, 1, &made, &stopped, error))
          return NULL;
        if (stopped)
          return stopped;
        r.backup = made;
        return report (&r, NULL);
      }
    case BRO_RIP_MODE_MKV:
      {
        g_autoptr (BroMakemkvSource) source = NULL;
        g_autoptr (GHashTable) map = g_hash_table_new (g_direct_hash, g_direct_equal);
        BroStepReport *stopped = NULL;
        if (!r.probe->listing)
          {
            *error = no_information ();
            return NULL;
          }
        if (bro_json_value_get_bool (profile_value (&r, "archive", "writeDiscInfo"), FALSE) && !write_disc_info (&r, r.probe->listing, error))
          return NULL;
        if (!(source = bro_step_sources_makemkv (r.request->source, context->drive_lookup, error)))
          return NULL;
        for (guint i = 0; i < r.plan->titles->len; i++)
          {
            int index = ((BroTitleChoice *) r.plan->titles->pdata[i])->index;
            g_hash_table_insert (map, GINT_TO_POINTER (index), GINT_TO_POINTER (index));
          }
        if (!rip (&r, source, r.probe->listing, map, BRO_ACQUIRE_STEP_TITLES_CHECKPOINT, 0, 1, &stopped, error))
          return NULL;
        return stopped ? stopped : report (&r, NULL);
      }
    case BRO_RIP_MODE_BACKUP_THEN_MKV:
      if (!r.probe->listing)
        {
          *error = no_information ();
          return NULL;
        }
      return backup_then_mkv (&r, r.probe->listing, resume_backup, error);
    default:
      {
        g_autofree char *detail = g_strconcat ("mode ", bro_rip_mode_to_wire (r.plan->mode), NULL);
        *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_INTERNAL_UNEXPECTED), _bro_param ("detail", bro_json_value_new_string (detail)), NULL);
        return NULL;
      }
    }
}

static void
step_iface_init (BroStepInterface *iface)
{
  iface->kind = step_kind;
  iface->resumability = step_resumability;
  iface->resume_checkpoint = step_resume_checkpoint;
  iface->resources = step_resources;
  iface->run = step_run;
}

static void
bro_acquire_step_class_init (BroAcquireStepClass *klass)
{
}

static void
bro_acquire_step_init (BroAcquireStep *self)
{
}

BroAcquireStep *
bro_acquire_step_new (void)
{
  return g_object_new (BRO_TYPE_ACQUIRE_STEP, NULL);
}
