/* bro-keep-apart-step.c */
#include "bro-keep-apart-step.h"
#include "bro-acquire-output.h"
#include "bro-archive-files.h"
#include "bro-archive-record-codec.h"
#include "bro-commit-output.h"
#include "bro-directory-entry.h"
#include "bro-disc-request.h"
#include "bro-file-info.h"
#include "bro-file-tree.h"
#include "bro-job-log.h"
#include "bro-notes.h"
#include "bro-plan-output.h"
#include "bro-probe-output.h"
#include "bro-sanitizer.h"
#include "bro-seal-record.h"
#include "bro-seal-step-private.h"
#include "bro-sha256-sums.h"
#include "bro-step-sources.h"
#include "bro-steps-private.h"
#include "bro-sum-entry.h"

#include <string.h>

#define QUARANTINE_FOLDER "_Quarantine"

struct _BroKeepApartStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroKeepApartStep, bro_keep_apart_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_KEEP_APART;
}

static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  BroResourceSet *set = bro_resource_set_new ();
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  const char *writes[] = { plan && plan->library_id && *plan->library_id ? plan->library_id : NULL, NULL };
  set->library_writes = g_strdupv ((char **) writes);
  return set;
}

static int
by_bytes (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

/* A component of the path starts with a dot. */
static gboolean
is_hidden (const char *f)
{
  g_auto (GStrv) parts = g_strsplit (f, "/", -1);
  for (guint i = 0; parts[i]; i++)
    if (parts[i][0] == '.')
      return TRUE;
  return FALSE;
}

/* Removes the folder's empty folders, and the folder itself once empty; a file stays. */
static gboolean
remove_empty (BroFileSystem *fs, const char *folder, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = bro_file_system_list (fs, folder, error);
  g_autoptr (GPtrArray) after = NULL;
  if (!entries)
    return FALSE;
  for (guint i = 0; i < entries->len; i++)
    {
      const BroDirectoryEntry *e = entries->pdata[i];
      g_autofree char *path = NULL;
      if (!e->is_directory)
        continue;
      path = g_strconcat (folder, "/", e->name, NULL);
      if (!remove_empty (fs, path, error))
        return FALSE;
    }
  if (!(after = bro_file_system_list (fs, folder, error)))
    return FALSE;
  return after->len > 0 || bro_file_system_remove (fs, folder, error);
}

/* The files (BroUnitFile *) and Bromelia's own files (char *) a "sealed" checkpoint names. */
static void
from_checkpoint (const BroJsonValue *saved, GPtrArray *files, GPtrArray *own)
{
  BroJsonValue *f = bro_json_value_member (saved, "files"), *o = bro_json_value_member (saved, "own");
  for (guint i = 0; i < bro_json_value_length (f); i++)
    {
      BroJsonValue *item = bro_json_value_at (f, i);
      BroUnitFile *file = bro_unit_file_new ();
      file->path = g_strdup (bro_json_value_get_string (bro_json_value_member (item, "path"), ""));
      file->size = bro_json_value_get_integer (bro_json_value_member (item, "size"), 0);
      file->sha256 = g_strdup (bro_json_value_get_string (bro_json_value_member (item, "sha256"), ""));
      file->role = g_strdup ("");
      g_ptr_array_add (files, file);
    }
  for (guint i = 0; i < bro_json_value_length (o); i++)
    g_ptr_array_add (own, g_strdup (bro_json_value_get_string (bro_json_value_at (o, i), "")));
}

/* {code, params, cause} (shared/schema/common.json BroError), params left out when empty. */
static BroJsonValue *
error_json (const BroBroError *error)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "code", bro_json_value_new_string (error->code));
  if (bro_json_value_length (error->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (error->params));
  if (error->cause)
    bro_json_value_set (o, "cause", error_json (error->cause));
  return o;
}

BroJsonValue *
bro_keep_apart_step_kept (BroOutcome outcome, const BroBroError *error)
{
  BroJsonValue *checkpoint = bro_json_value_new_object ();
  bro_json_value_set (checkpoint, "kind", bro_json_value_new_string (BRO_KEEP_APART_STEP_OUTCOME_CHECKPOINT));
  bro_json_value_set (checkpoint, "outcome", bro_json_value_new_string (bro_outcome_to_wire (outcome)));
  if (error)
    bro_json_value_set (checkpoint, "error", error_json (error));
  return checkpoint;
}

char *
bro_keep_apart_step_folder_name (const BroJobSnapshot *job)
{
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  BroJsonValue *probe_json = bro_job_snapshot_output (job, BRO_STEP_KIND_PROBE);
  g_autoptr (BroProbeOutput) probe = probe_json && probe_json->kind == BRO_JSON_VALUE_OBJECT ? bro_probe_output_decode (probe_json) : NULL;
  g_autoptr (BroDiscRequest) request = bro_disc_request_decode (job->request);
  g_autofree char *source_label = request && request->source ? bro_step_sources_label (request->source, NULL) : g_strdup ("");
  const char *candidates[] = { plan && plan->identity ? plan->identity->name : NULL, job->request->title,
                               probe && probe->listing ? probe->listing->name : NULL, source_label, job->id.value };
  for (guint i = 0; i < G_N_ELEMENTS (candidates); i++)
    {
      char *n = bro_sanitizer_component (candidates[i] ? candidates[i] : "");
      if (*n)
        return n;
      g_free (n);
    }
  return g_strdup ("");
}

/* Seals what staging holds into @files (BroUnitFile *) and @own (char *: the record first, the logs, the note), once the
 * checkpoint that names them is stored; neither filled when staging holds no file to keep (its empty folders removed). */
static gboolean
seal (const BroStepContext *c, const BroPlanOutput *plan, const char *staging, const BroId *unit_id, GPtrArray *files, GPtrArray *own,
      BroBroError **error)
{
  BroFileSystem *fs = c->fs;
  BroJsonValue *archive = bro_json_value_member (plan->profile, "archive");
  gboolean hash_from_disk = bro_json_value_get_bool (bro_json_value_member (archive, "hashFromDisk"), TRUE);
  gboolean copy_logs = bro_json_value_get_bool (bro_json_value_member (archive, "archiveRecord"), TRUE);
  g_autoptr (GPtrArray) walked = NULL;
  g_autoptr (GPtrArray) tree = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) folders = NULL;
  g_autoptr (GArray) sizes = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_autoptr (GPtrArray) written = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) logs = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) log = NULL;
  g_autoptr (GPtrArray) log_names = g_ptr_array_new ();
  g_autoptr (GPtrArray) lines = NULL;
  g_autoptr (BroAcquireOutput) acquired = NULL;
  g_autoptr (BroJsonValue) document = NULL;
  g_autoptr (BroJsonValue) checkpoint = NULL;
  g_autofree char *record = NULL;
  BroJsonValue *checkpoint_files, *checkpoint_own;
  gint64 total = 0, done = 0;
  char unit8[9];
  BroSealRun run = { c, staging, written };

  if (!(walked = bro_file_tree_files (fs, staging, error)))
    return FALSE;
  for (guint i = 0; i < walked->len; i++)
    {
      const char *f = walked->pdata[i];
      const char *slash = strrchr (f, '/');
      if (!g_str_has_suffix (f, "/") && bro_archive_files_is_leftover (slash ? slash + 1 : f))
        {
          g_autofree char *path = g_strconcat (staging, "/", f, NULL);
          if (!bro_file_system_remove (fs, path, error))
            return FALSE;
        }
      else
        g_ptr_array_add (tree, g_strdup (f));
    }
  for (guint i = 0; i < tree->len; i++)
    {
      const char *f = tree->pdata[i];
      if (!g_str_has_suffix (f, "/") && !is_hidden (f) && !(!strchr (f, '/') && bro_archive_files_is_own_file (f)))
        g_ptr_array_add (paths, g_strdup (f));
    }
  if (paths->len == 0)
    return remove_empty (fs, staging, error);

  if (_bro_seal_cancelled (c, error))
    return FALSE;
  for (guint i = 0; i < paths->len; i++)
    {
      g_autofree char *path = g_strconcat (staging, "/", (char *) paths->pdata[i], NULL);
      if (!bro_file_system_sync_file (fs, path, error))
        return FALSE;
    }
  folders = _bro_seal_folders (staging, paths);
  for (guint i = 0; i < folders->len; i++)
    if (!bro_file_system_sync_directory (fs, folders->pdata[i], error))
      return FALSE;
  _bro_step_log (c, BRO_MSG_ARCHIVE_SYNCED, _bro_param ("count", bro_json_value_new_integer (paths->len)), BRO_SEVERITY_INFO);

  for (guint i = 0; i < paths->len; i++)
    {
      g_autofree char *path = g_strconcat (staging, "/", (char *) paths->pdata[i], NULL);
      g_autoptr (BroFileInfo) info = bro_file_system_stat (fs, path, error);
      if (!info)
        return FALSE;
      g_array_append_val (sizes, info->size);
      total += info->size;
    }
  total = MAX (1, total);
  for (guint i = 0; i < paths->len; i++)
    {
      const char *file = paths->pdata[i];
      g_autofree char *path = g_strconcat (staging, "/", file, NULL);
      g_autoptr (BroBroMessage) operation = bro_bro_message_new (BRO_MSG_ARCHIVE_HASHING, NULL, BRO_SEVERITY_INFO);
      BroUnitFile *entry;
      gint64 size = 0;
      char *digest;
      if (_bro_seal_cancelled (c, error))
        return FALSE;
      bro_progress_reporter_report (c->progress, (double) done / total, operation, file);
      if (!(digest = _bro_seal_hash (c, path, file, hash_from_disk, &size, error)))
        return FALSE;
      entry = bro_unit_file_new ();
      entry->path = g_strdup (file);
      entry->size = size;
      entry->sha256 = digest;
      entry->role = g_strdup ("");
      g_ptr_array_add (files, entry);
      done += g_array_index (sizes, gint64, i);
    }
  if (_bro_seal_cancelled (c, error))
    return FALSE;

  g_strlcpy (unit8, unit_id->value, sizeof unit8);
  {
    g_autoptr (GPtrArray) sums = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_sum_entry_free);
    g_autofree char *text = NULL;
    BroJsonValue *params;
    for (guint i = 0; i < files->len; i++)
      g_ptr_array_add (sums, bro_sum_entry_new (((BroUnitFile *) files->pdata[i])->path, ((BroUnitFile *) files->pdata[i])->sha256));
    text = bro_sha256_sums_render (sums);
    if (!_bro_seal_write_text (&run, BRO_SHA256_SUMS_FILE_NAME, text, error))
      return FALSE;
    params = _bro_param ("count", bro_json_value_new_integer (files->len));
    bro_json_value_set (params, "file", bro_json_value_new_string (BRO_SHA256_SUMS_FILE_NAME));
    _bro_step_log (c, BRO_MSG_ARCHIVE_CHECKSUMS_WRITTEN, params, BRO_SEVERITY_INFO);
  }
  if (!(log = _bro_seal_copy_logs (&run, unit8, copy_logs, logs, error)))
    return FALSE;
  for (guint i = 0; i < logs->len; i++)
    g_ptr_array_add (log_names, logs->pdata[i]);
  g_ptr_array_add (log_names, NULL);
  {
    /* The outcome and error JobService.keepApart stored with the step; interrupted and none without them. */
    g_autoptr (BroJsonValue) saved = bro_checkpointer_saved (c->checkpoint);
    BroJsonValue *kept = saved && g_strcmp0 (bro_json_value_get_string (bro_json_value_member (saved, "kind"), NULL),
                                             BRO_KEEP_APART_STEP_OUTCOME_CHECKPOINT) == 0 ? saved : NULL;
    BroJsonValue *e = bro_json_value_member (kept, "error");
    const char *code = bro_json_value_get_string (bro_json_value_member (e, "code"), NULL);
    BroJsonValue *params = bro_json_value_member (e, "params");
    g_autoptr (BroBroError) kept_error = NULL;
    BroOutcome outcome = BRO_OUTCOME_INTERRUPTED;
    bro_outcome_from_wire (bro_json_value_get_string (bro_json_value_member (kept, "outcome"), NULL), &outcome);
    if (code)
      kept_error = bro_bro_error_new (code, params && params->kind == BRO_JSON_VALUE_OBJECT ? bro_json_value_ref (params) : bro_json_value_new_object (),
                                      NULL);
    g_autofree char *full_log = g_strconcat (c->job_directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
    lines = bro_notes_render (&c->job->id, outcome, kept_error, NULL, (const char *const *) log_names->pdata, full_log);
  }
  {
    GString *text = g_string_new ("");
    g_autofree char *note = NULL;
    for (guint i = 0; i < lines->len; i++)
      {
        g_autofree char *rendered = bro_message_renderer_render (c->messages, lines->pdata[i]);
        g_string_append (text, rendered);
        g_string_append_c (text, '\n');
      }
    note = g_string_free (text, FALSE);
    if (!_bro_seal_write_text (&run, BRO_KEEP_APART_STEP_NOTE, note, error))
      return FALSE;
  }
  record = g_strdup_printf ("bromelia-%s.json", unit8);
  acquired = bro_acquire_output_new ();
  document = bro_seal_record_build (c->job, plan, acquired, unit_id, files, log, logs, bro_clock_now (c->clock), c->app_version, c->messages,
                                    "incomplete");
  {
    BroArchiveRecord r = { 0 };
    g_autoptr (GBytes) bytes = NULL;
    r.version = 3;
    r.document = document;
    bytes = bro_archive_record_codec_encode_v3 (&r);
    if (!_bro_seal_write (&run, record, bytes, error))
      return FALSE;
  }
  _bro_step_log (c, BRO_MSG_ARCHIVE_RECORD_WRITTEN, _bro_param ("file", bro_json_value_new_string (record)), BRO_SEVERITY_INFO);
  for (guint i = 0; i < tree->len; i++)
    {
      const char *f = tree->pdata[i];
      if (!strchr (f, '/') && bro_archive_files_is_own_file (f) && !_bro_seal_was_written (&run, f))
        {
          g_autofree char *path = g_strconcat (staging, "/", f, NULL);
          if (!bro_file_system_remove (fs, path, error))
            return FALSE;
        }
    }
  if (!bro_file_system_sync_directory (fs, staging, error))
    return FALSE;

  g_ptr_array_add (own, g_strdup (record));
  for (guint i = 0; i < logs->len; i++)
    g_ptr_array_add (own, g_strdup (logs->pdata[i]));
  g_ptr_array_add (own, g_strdup (BRO_KEEP_APART_STEP_NOTE));
  checkpoint = bro_json_value_new_object ();
  checkpoint_files = bro_json_value_new_array ();
  checkpoint_own = bro_json_value_new_array ();
  for (guint i = 0; i < files->len; i++)
    {
      const BroUnitFile *f = files->pdata[i];
      BroJsonValue *item = _bro_param ("path", bro_json_value_new_string (f->path));
      bro_json_value_set (item, "size", bro_json_value_new_integer (f->size));
      bro_json_value_set (item, "sha256", bro_json_value_new_string (f->sha256));
      bro_json_value_append (checkpoint_files, item);
    }
  for (guint i = 0; i < own->len; i++)
    bro_json_value_append (checkpoint_own, bro_json_value_new_string (own->pdata[i]));
  bro_json_value_set (checkpoint, "kind", bro_json_value_new_string (BRO_KEEP_APART_STEP_SEALED_CHECKPOINT));
  bro_json_value_set (checkpoint, "files", checkpoint_files);
  bro_json_value_set (checkpoint, "own", checkpoint_own);
  return bro_checkpointer_save (c->checkpoint, checkpoint, error);
}

static BroCommitItem *
item_new (int seq, const char *path, const char *sha256, gboolean is_dir)
{
  BroCommitItem *item = bro_commit_item_new ();
  item->seq = seq;
  item->from_path = g_strdup (path);
  item->to_path = g_strdup (path);
  item->sha256 = g_strdup (sha256);
  item->is_dir = is_dir;
  return item;
}

/* Staging's top-level entries that hold the files, in byte order (a file with its SHA-256, a folder without), then
 * Bromelia's own files; each in place. Into @items. */
static void
add_items (GPtrArray *items, GPtrArray *files, GPtrArray *own)
{
  g_autoptr (GPtrArray) tops = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < files->len; i++)
    {
      const char *path = ((BroUnitFile *) files->pdata[i])->path;
      const char *slash = strchr (path, '/');
      char *top = slash ? g_strndup (path, slash - path) : g_strdup (path);
      gboolean seen = FALSE;
      for (guint k = 0; k < tops->len && !seen; k++)
        seen = strcmp (tops->pdata[k], top) == 0;
      if (seen)
        g_free (top);
      else
        g_ptr_array_add (tops, top);
    }
  g_ptr_array_sort (tops, by_bytes);
  for (guint i = 0; i < tops->len; i++)
    {
      const BroUnitFile *file = NULL;
      for (guint j = 0; j < files->len && !file; j++)
        if (strcmp (((BroUnitFile *) files->pdata[j])->path, tops->pdata[i]) == 0)
          file = files->pdata[j];
      g_ptr_array_add (items, item_new ((int) items->len, tops->pdata[i], file ? file->sha256 : NULL, file == NULL));
    }
  for (guint i = 0; i < own->len; i++)
    g_ptr_array_add (items, item_new ((int) items->len, own->pdata[i], NULL, FALSE));
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  const BroStepContext *c = context;
  const BroJobSnapshot *job = c->job;
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  g_autoptr (BroLibraryHandle) library = NULL;
  g_autofree char *staging = NULL;
  g_autoptr (BroJsonValue) saved = NULL;
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  g_autoptr (GPtrArray) own = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroProbeOutput) probe = NULL;
  g_autoptr (BroDiscRequest) request = NULL;
  g_autofree char *source_label = NULL;
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  g_autofree char *root_prefix = NULL;
  g_autoptr (BroCommitIntent) intent = NULL;
  g_autoptr (BroUnitRecord) unit = NULL;
  g_autoptr (BroCommitResult) result = NULL;
  g_autoptr (BroCommitOutput) output = NULL;
  const BroIdentity *identity;
  BroJsonValue *probe_json;
  const char *listing_name;
  BroInstant now;
  BroId unit_id;

  if (!plan)
    return _bro_step_skipped ();
  if (!(library = bro_library_port_require (c->library, plan->library_id, error)) || _bro_seal_cancelled (c, error))
    return NULL;
  if (!(staging = bro_library_port_staging_directory (c->library, plan->library_id, &job->id, error)))
    return NULL;
  unit_id = bro_archive_files_unit_id (&job->id);

  /* The unit an earlier attempt sealed; one that names none of Bromelia's own files isn't (nothing writes one). */
  saved = bro_checkpointer_saved (c->checkpoint);
  if (saved && g_strcmp0 (bro_json_value_get_string (bro_json_value_member (saved, "kind"), NULL), BRO_KEEP_APART_STEP_SEALED_CHECKPOINT) == 0
      && bro_json_value_length (bro_json_value_member (saved, "own")) > 0)
    from_checkpoint (saved, files, own);
  else if (!seal (c, plan, staging, &unit_id, files, own, error))
    return NULL;
  if (files->len == 0)
    return bro_step_report_new (NULL);

  probe_json = bro_job_snapshot_output (job, BRO_STEP_KIND_PROBE);
  probe = probe_json && probe_json->kind == BRO_JSON_VALUE_OBJECT ? bro_probe_output_decode (probe_json) : NULL;
  request = bro_disc_request_decode (job->request);
  source_label = request && request->source ? bro_step_sources_label (request->source, NULL) : g_strdup ("");
  listing_name = probe && probe->listing && probe->listing->name ? probe->listing->name : "";
  identity = plan->identity;
  name = bro_keep_apart_step_folder_name (job);
  path = g_strconcat (QUARANTINE_FOLDER "/", name, NULL);
  now = bro_clock_now (c->clock);

  unit = bro_unit_record_new ();
  unit->id = unit_id;
  unit->library_id = g_strdup (plan->library_id);
  unit->has_job_id = TRUE;
  unit->job_id = job->id;
  unit->path = g_strdup (path);
  unit->record_file = g_strdup (own->pdata[0]);
  unit->record_version = 3;
  unit->state = BRO_UNIT_STATE_COMMITTING;
  unit->status = BRO_UNIT_STATUS_INCOMPLETE;
  unit->name = g_strdup (identity->name);
  unit->kind = identity->kind;
  unit->format = g_strdup (bro_disc_format_to_wire (identity->format));
  unit->format_code = g_strdup (identity->format_code.text);
  unit->encrypted = identity->encrypted;
  unit->fingerprint = g_strdup (probe ? probe->fingerprint : NULL);
  unit->label = g_strdup (*listing_name ? listing_name : source_label);
  unit->has_season = identity->label->season >= 0;
  unit->season = identity->label->season;
  unit->has_part = identity->label->part >= 0;
  unit->part = identity->label->part;
  unit->has_volume = identity->label->volume >= 0;
  unit->volume = identity->label->volume;
  unit->has_disc = identity->label->disc >= 0;
  unit->disc = identity->label->disc;
  unit->makemkv_version = g_strdup (probe && probe->makemkv_version ? probe->makemkv_version : "");
  for (guint i = 0; i < files->len; i++)
    unit->bytes += ((BroUnitFile *) files->pdata[i])->size;
  unit->file_count = (int) files->len;
  unit->attempts = 1;
  unit->created_at = now;

  intent = bro_commit_intent_new ();
  intent->unit_id = unit_id;
  intent->job_id = job->id;
  intent->staging = g_strdup (staging);
  intent->destination = g_strconcat (library->path, "/", path, NULL);
  intent->created_at = now;
  add_items (intent->items, files, own);

  _bro_step_progress (c, 0, BRO_MSG_ARCHIVE_MOVING);
  if (!(result = bro_library_port_quarantine (c->library, unit, intent, files, BRO_KEEP_APART_STEP_TAG, c->cancel, error)))
    return NULL;
  _bro_step_log (c, BRO_MSG_RECOVERY_KEPT_APART, _bro_param ("path", bro_json_value_new_string (result->destination)), BRO_SEVERITY_WARNING);
  root_prefix = g_strconcat (library->path, "/", NULL);
  output = bro_commit_output_new ();
  output->unit_id = unit_id;
  g_free (output->library);
  output->library = g_strdup (plan->library_id);
  g_free (output->path);
  output->path = g_strdup (g_str_has_prefix (result->destination, root_prefix) ? result->destination + strlen (root_prefix) : result->destination);
  output->merged = FALSE;
  output->quarantined = TRUE;
  return bro_step_report_new (bro_commit_output_encode (output));
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
bro_keep_apart_step_class_init (BroKeepApartStepClass *klass)
{
}

static void
bro_keep_apart_step_init (BroKeepApartStep *self)
{
}

BroKeepApartStep *
bro_keep_apart_step_new (void)
{
  return g_object_new (BRO_TYPE_KEEP_APART_STEP, NULL);
}
