/* bro-seal-step.c */
#include "bro-seal-step.h"
#include "bro-acquire-output.h"
#include "bro-archive-files.h"
#include "bro-archive-record-codec.h"
#include "bro-file-info.h"
#include "bro-file-tree.h"
#include "bro-job-log.h"
#include "bro-log-line-dto.h"
#include "bro-name-output.h"
#include "bro-notes.h"
#include "bro-plan-output.h"
#include "bro-robot-message.h"
#include "bro-seal-output.h"
#include "bro-seal-record.h"
#include "bro-sha256-hasher.h"
#include "bro-sha256-sums.h"
#include "bro-steps-private.h"
#include "bro-sum-entry.h"

#include <string.h>

#define CHUNK (1 << 20)
#define FILE_MODE 0644

struct _BroSealStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroSealStep, bro_seal_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_SEAL;
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

/* ---- failures ---- */

/* job.cancelled once the job is cancelled. */
static gboolean
cancelled (const BroStepContext *c, BroBroError **error)
{
  if (!bro_cancellation_token_is_cancelled (c->cancel))
    return FALSE;
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
  return TRUE;
}

/* @code with @params (taken), caused by @cause (taken, nullable). */
static BroBroError *
failure (BroMessageCode code, BroJsonValue *params, BroBroError *cause)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, params, BRO_SEVERITY_ERROR);
  return bro_bro_message_to_error (m, cause);
}

/* An error's text in the configured language. */
static char *
text_of (const BroStepContext *c, const BroBroError *error)
{
  BroMessageCode code;
  g_autoptr (BroBroMessage) m = NULL;
  if (!bro_message_code_parse (error->code, &code))
    code = BRO_MSG_INTERNAL_UNEXPECTED;
  m = bro_bro_message_new (code, bro_json_value_ref (error->params), BRO_SEVERITY_ERROR);
  return bro_message_renderer_render (c->messages, m);
}

/* {key1: value1, key2: @reason's text}; takes @value1 and @reason (the cause). */
static BroBroError *
with_reason (const BroStepContext *c, BroMessageCode code, const char *key, BroJsonValue *value, BroBroError *reason)
{
  g_autofree char *text = text_of (c, reason);
  BroJsonValue *params = _bro_param (key, value);
  bro_json_value_set (params, "reason", bro_json_value_new_string (text));
  return failure (code, params, reason);
}

/* ---- hashing ---- */

/* A file's SHA-256 (g_free it) and size, read through the FileSystem port (from disk when @from_disk);
 * archive.hashFailed when it can't be read; job.cancelled between two pieces. NULL and @error set on failure. */
static char *
hash (const BroStepContext *c, const char *path, const char *file, gboolean from_disk, gint64 *size, BroBroError **error)
{
  BroBroError *cause = NULL;
  g_autoptr (BroByteStream) stream = bro_file_system_open_for_reading (c->fs, path, from_disk, &cause);
  g_autoptr (BroSha256Hasher) hasher = NULL;
  char *digest = NULL;
  if (!stream)
    {
      *error = with_reason (c, BRO_MSG_ARCHIVE_HASH_FAILED, "file", bro_json_value_new_string (file), cause);
      return NULL;
    }
  hasher = bro_sha256_hasher_new ();
  *size = 0;
  while (!cancelled (c, error))
    {
      g_autoptr (GBytes) piece = bro_byte_stream_read (stream, CHUNK, &cause);
      gsize n;
      const guint8 *bytes;
      if (!piece)
        {
          *error = with_reason (c, BRO_MSG_ARCHIVE_HASH_FAILED, "file", bro_json_value_new_string (file), cause);
          break;
        }
      bytes = g_bytes_get_data (piece, &n);
      if (n == 0)
        {
          digest = bro_sha256_hasher_finish (hasher);
          break;
        }
      bro_sha256_hasher_update (hasher, bytes, n);
      *size += n;
    }
  bro_byte_stream_close (stream);
  return digest;
}

/* ---- the step ---- */

char *
bro_seal_step_log_text (GPtrArray *lines, BroMessageRenderer *messages)
{
  GString *text = g_string_new ("");
  for (guint i = 0; i < lines->len; i++)
    {
      const BroLogLineDto *l = lines->pdata[i];
      g_autofree char *when = bro_instant_format (l->t);
      g_autoptr (BroBroMessage) m = bro_bro_message_new (l->code, l->params ? bro_json_value_ref (l->params) : NULL, l->severity);
      g_autofree char *rendered = bro_message_renderer_render (messages, m);
      g_string_append (text, when);
      g_string_append_c (text, ' ');
      if (l->severity != BRO_SEVERITY_INFO)
        {
          g_autofree char *tag = g_ascii_strup (bro_severity_to_wire (l->severity), -1);
          g_string_append_printf (text, "[%s] ", tag);
        }
      g_string_append (text, rendered);
      g_string_append_c (text, '\n');
    }
  return g_string_free (text, FALSE);
}

static gboolean
is_folder (const char *f)
{
  return g_str_has_suffix (f, "/");
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

static char *
trimmed (const char *path)
{
  char *t = g_strdup (path);
  gsize n = strlen (t);
  while (n > 0 && t[n - 1] == '/')
    t[--n] = '\0';
  return t;
}

/* The named item @file is (or is in); NULL when none is (never: every file comes from the named items). */
static const BroNamedFile *
item_of (GPtrArray *named, const char *file)
{
  for (guint i = 0; i < named->len; i++)
    {
      const BroNamedFile *n = named->pdata[i];
      g_autofree char *p = trimmed (n->path);
      gsize len = strlen (p);
      if (strcmp (file, p) == 0 || (strncmp (file, p, len) == 0 && file[len] == '/'))
        return n;
    }
  return NULL;
}

static int
by_bytes (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

/* What one run of the step has. */
typedef struct {
  const BroStepContext *c;
  const char *staging;
  GPtrArray *written; /* char *: Bromelia's own files this run wrote */
} Run;

/* Writes @name into staging atomically (0644); archive.writeFailed with the cause. */
static gboolean
write_own (Run *r, const char *name, GBytes *bytes, BroBroError **error)
{
  g_autofree char *path = g_strconcat (r->staging, "/", name, NULL);
  BroBroError *cause = NULL;
  if (!bro_file_system_write_atomically (r->c->fs, path, bytes, FILE_MODE, &cause))
    {
      *error = with_reason (r->c, BRO_MSG_ARCHIVE_WRITE_FAILED, "path", bro_json_value_new_string (path), cause);
      return FALSE;
    }
  g_ptr_array_add (r->written, g_strdup (name));
  return TRUE;
}

static gboolean
write_text (Run *r, const char *name, const char *text, BroBroError **error)
{
  g_autoptr (GBytes) bytes = g_bytes_new (text, strlen (text));
  return write_own (r, name, bytes, error);
}

static gboolean
was_written (Run *r, const char *name)
{
  for (guint i = 0; i < r->written->len; i++)
    if (strcmp (r->written->pdata[i], name) == 0)
      return TRUE;
  return FALSE;
}

/* The READ ERRORS note's text: the job, its outcome, rip.readErrors, the read errors (titles', then the run-level ones)
 * and the copied logs, one rendered message per line. */
static char *
note_text (const BroStepContext *c, const BroAcquireOutput *acquired, int read_errors, GPtrArray *logs)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_robot_message_free);
  g_autoptr (GPtrArray) log_names = g_ptr_array_new ();
  g_autoptr (BroBroError) error = failure (BRO_MSG_RIP_READ_ERRORS, _bro_param ("count", bro_json_value_new_integer (read_errors)), NULL);
  g_autoptr (GPtrArray) lines = NULL;
  GString *text = g_string_new ("");
  for (guint i = 0; i <= acquired->titles->len; i++)
    {
      GPtrArray *texts = i < acquired->titles->len ? ((BroAcquiredTitle *) acquired->titles->pdata[i])->read_errors : acquired->read_errors;
      for (guint j = 0; j < texts->len; j++)
        g_ptr_array_add (errors, bro_robot_message_new (2003, 0, texts->pdata[j]));
    }
  for (guint i = 0; i < logs->len; i++)
    g_ptr_array_add (log_names, logs->pdata[i]);
  g_ptr_array_add (log_names, NULL);
  lines = bro_notes_render (&c->job->id, BRO_OUTCOME_SUCCEEDED_WITH_READ_ERRORS, error, errors, (const char *const *) log_names->pdata);
  for (guint i = 0; i < lines->len; i++)
    {
      g_autofree char *rendered = bro_message_renderer_render (c->messages, lines->pdata[i]);
      g_string_append (text, rendered);
      g_string_append_c (text, '\n');
    }
  return g_string_free (text, FALSE);
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  const BroStepContext *c = context;
  BroFileSystem *fs = c->fs;
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (c->job);
  g_autoptr (BroAcquireOutput) acquired = bro_acquire_output_of (c->job);
  g_autoptr (BroNameOutput) name = NULL;
  g_autofree char *staging = NULL;
  g_autoptr (GPtrArray) named = NULL;
  g_autoptr (GPtrArray) walked = NULL;
  g_autoptr (GPtrArray) tree = g_ptr_array_new_with_free_func (g_free); /* the walk without the leftovers removed */
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GHashTable) sealed = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) folders = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GArray) sizes = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_autoptr (GPtrArray) entries = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  g_autoptr (GPtrArray) written = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) logs = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) log = NULL;
  g_autoptr (BroSealOutput) output = NULL;
  g_autoptr (BroJsonValue) document = NULL;
  g_autofree char *log_file = NULL;
  g_autofree char *note = NULL;
  BroJsonValue *archive, *checksums;
  gboolean hash_from_disk, copy_logs;
  gint64 total = 0, done = 0;
  int read_errors;
  BroId unit_id;
  char unit8[9];
  Run run;

  if (!plan || !acquired)
    return _bro_step_skipped ();
  if (!(staging = bro_library_port_staging_directory (c->library, plan->library_id, &c->job->id, error)))
    return NULL;
  run.c = c;
  run.staging = staging;
  run.written = written;
  archive = bro_json_value_member (plan->profile, "archive");
  hash_from_disk = bro_json_value_get_bool (bro_json_value_member (archive, "hashFromDisk"), TRUE);
  copy_logs = bro_json_value_get_bool (bro_json_value_member (archive, "archiveRecord"), TRUE);
  checksums = bro_json_value_member (archive, "checksums");
  if (checksums && checksums->kind == BRO_JSON_VALUE_BOOL && !checksums->boolean)
    _bro_step_log (c, BRO_MSG_ARCHIVE_CHECKSUMS_REQUIRED, NULL, BRO_SEVERITY_WARNING);

  /* What is to be sealed, and nothing else. */
  name = bro_name_output_of (c->job);
  named = name ? g_ptr_array_ref (name->files) : bro_name_output_from_acquire (acquired);
  for (guint i = 0; i < named->len; i++)
    {
      const BroNamedFile *item = named->pdata[i];
      g_autofree char *item_path = trimmed (item->path);
      g_autofree char *path = g_strconcat (staging, "/", item_path, NULL);
      if (!bro_file_system_exists (fs, path))
        {
          *error = failure (BRO_MSG_ARCHIVE_FILE_MISSING, _bro_param ("file", bro_json_value_new_string (item->path)), NULL);
          return NULL;
        }
    }
  if (!(walked = bro_file_tree_files (fs, staging, error)))
    return NULL;
  for (guint i = 0; i < walked->len; i++)
    {
      const char *f = walked->pdata[i];
      const char *slash = strrchr (f, '/');
      if (!is_folder (f) && bro_archive_files_is_leftover (slash ? slash + 1 : f))
        {
          g_autofree char *path = g_strconcat (staging, "/", f, NULL);
          if (!bro_file_system_remove (fs, path, error))
            return NULL;
        }
      else
        g_ptr_array_add (tree, g_strdup (f));
    }
  {
    g_autoptr (GPtrArray) produced = g_ptr_array_new ();
    g_auto (GStrv) expanded = NULL;
    for (guint i = 0; i < named->len; i++)
      g_ptr_array_add (produced, ((BroNamedFile *) named->pdata[i])->path);
    g_ptr_array_add (produced, NULL);
    g_ptr_array_add (tree, NULL);
    expanded = bro_archive_files_expand ((const char *const *) produced->pdata, (const char *const *) tree->pdata);
    g_ptr_array_remove_index (tree, tree->len - 1);
    for (guint i = 0; expanded[i]; i++)
      if (!is_folder (expanded[i]))
        g_ptr_array_add (files, g_strdup (expanded[i]));
  }
  for (guint i = 0; i < files->len; i++)
    g_hash_table_add (sealed, files->pdata[i]);
  for (guint i = 0; i < tree->len; i++)
    {
      const char *f = tree->pdata[i];
      gboolean own = !strchr (f, '/') && bro_archive_files_is_own_file (f);
      if (!is_folder (f) && !is_hidden (f) && !own && !g_hash_table_contains (sealed, f))
        {
          *error = failure (BRO_MSG_ARCHIVE_UNLISTED, _bro_param ("file", bro_json_value_new_string (f)), NULL);
          return NULL;
        }
    }

  /* 1. Durable before it is read back. */
  if (cancelled (c, error))
    return NULL;
  for (guint i = 0; i < files->len; i++)
    {
      const char *f = files->pdata[i];
      g_autofree char *path = g_strconcat (staging, "/", f, NULL);
      gsize n = 0;
      if (!bro_file_system_sync_file (fs, path, error))
        return NULL;
      do /* every folder from staging down to the file's: one holding only folders (a backup's) too */
        {
          const char *slash = strchr (f + n, '/');
          char *folder = n == 0 ? g_strdup (staging) : g_strdup_printf ("%s/%.*s", staging, (int) (n - 1), f);
          gboolean seen = FALSE;
          for (guint k = 0; k < folders->len && !seen; k++)
            seen = strcmp (folders->pdata[k], folder) == 0;
          if (seen)
            g_free (folder);
          else
            g_ptr_array_add (folders, folder);
          n = slash ? (gsize) (slash - f) + 1 : 0;
        }
      while (n > 0);
    }
  g_ptr_array_sort (folders, by_bytes);
  for (guint i = 0; i < folders->len; i++)
    if (!bro_file_system_sync_directory (fs, folders->pdata[i], error))
      return NULL;
  _bro_step_log (c, BRO_MSG_ARCHIVE_SYNCED, _bro_param ("count", bro_json_value_new_integer (files->len)), BRO_SEVERITY_INFO);

  /* 2. Hashes of what is on disk. */
  for (guint i = 0; i < files->len; i++)
    {
      g_autofree char *path = g_strconcat (staging, "/", (char *) files->pdata[i], NULL);
      g_autoptr (BroFileInfo) info = bro_file_system_stat (fs, path, error);
      if (!info)
        return NULL;
      g_array_append_val (sizes, info->size);
      total += info->size;
    }
  total = MAX (1, total);
  for (guint i = 0; i < files->len; i++)
    {
      const char *file = files->pdata[i];
      g_autofree char *path = g_strconcat (staging, "/", file, NULL);
      g_autoptr (BroBroMessage) operation = bro_bro_message_new (BRO_MSG_ARCHIVE_HASHING, NULL, BRO_SEVERITY_INFO);
      const BroNamedFile *item;
      BroUnitFile *entry;
      gint64 size = 0;
      char *digest;
      if (cancelled (c, error))
        return NULL;
      bro_progress_reporter_report (c->progress, (double) done / total, operation, file);
      if (!(digest = hash (c, path, file, hash_from_disk, &size, error)))
        return NULL;
      item = item_of (named, file);
      entry = bro_unit_file_new ();
      entry->path = g_strdup (file);
      entry->size = size;
      entry->sha256 = digest;
      entry->role = g_strdup (item && item->role ? item->role : "");
      entry->has_title = item && item->has_title;
      entry->title = entry->has_title ? item->title : 0;
      g_ptr_array_add (entries, entry);
      done += g_array_index (sizes, gint64, i);
    }
  if (cancelled (c, error))
    return NULL;

  /* 3. Bromelia's own files, each written atomically. */
  unit_id = bro_archive_files_unit_id (&c->job->id);
  g_strlcpy (unit8, unit_id.value, sizeof unit8);
  {
    g_autoptr (GPtrArray) sums = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_sum_entry_free);
    g_autofree char *text = NULL;
    for (guint i = 0; i < entries->len; i++)
      g_ptr_array_add (sums, bro_sum_entry_new (((BroUnitFile *) entries->pdata[i])->path, ((BroUnitFile *) entries->pdata[i])->sha256));
    text = bro_sha256_sums_render (sums);
    if (!write_text (&run, BRO_SHA256_SUMS_FILE_NAME, text, error))
      return NULL;
  }
  {
    BroJsonValue *params = _bro_param ("count", bro_json_value_new_integer (entries->len));
    bro_json_value_set (params, "file", bro_json_value_new_string (BRO_SHA256_SUMS_FILE_NAME));
    _bro_step_log (c, BRO_MSG_ARCHIVE_CHECKSUMS_WRITTEN, params, BRO_SEVERITY_INFO);
  }

  log_file = g_strconcat (c->job_directory, "/" BRO_JOB_LOG_FILE_NAME, NULL);
  if (bro_file_system_exists (fs, log_file))
    {
      g_autoptr (GBytes) bytes = bro_file_system_read (fs, log_file, error);
      if (!bytes)
        return NULL;
      log = bro_job_log_decode (bytes);
    }
  else
    log = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_log_line_dto_free);
  if (copy_logs)
    {
      const char *from[] = { BRO_SEAL_STEP_TRANSCRIPT, BRO_SEAL_STEP_DEBUG_TRANSCRIPT };
      const char *to[] = { "makemkv-%s-log.txt", "makemkv-%s-debug-log.txt" };
      if (bro_file_system_exists (fs, log_file))
        {
          g_autofree char *copy = g_strdup_printf ("bromelia-%s-log.txt", unit8);
          g_autofree char *text = bro_seal_step_log_text (log, c->messages);
          if (!write_text (&run, copy, text, error))
            return NULL;
          g_ptr_array_add (logs, g_steal_pointer (&copy));
        }
      for (guint i = 0; i < G_N_ELEMENTS (from); i++)
        {
          g_autofree char *source = g_strconcat (c->job_directory, "/", from[i], NULL);
          g_autofree char *copy = g_strdup_printf (to[i], unit8);
          g_autoptr (GBytes) bytes = NULL;
          if (!bro_file_system_exists (fs, source))
            continue;
          if (!(bytes = bro_file_system_read (fs, source, error)) || !write_own (&run, copy, bytes, error))
            return NULL;
          g_ptr_array_add (logs, g_steal_pointer (&copy));
        }
    }

  read_errors = bro_acquire_output_read_error_count (acquired);
  if (read_errors > 0)
    {
      g_autofree char *text = note_text (c, acquired, read_errors, logs);
      if (!write_text (&run, BRO_SEAL_STEP_READ_ERRORS_NOTE, text, error))
        return NULL;
      note = g_strdup (BRO_SEAL_STEP_READ_ERRORS_NOTE);
    }

  output = bro_seal_output_new ();
  output->unit_id = unit_id;
  g_free (output->record);
  output->record = g_strdup_printf ("bromelia-%s.json", unit8);
  document = bro_seal_record_build (c->job, plan, acquired, &unit_id, entries, log, logs, bro_clock_now (c->clock), c->app_version, c->messages);
  {
    BroArchiveRecord record = { 0 };
    g_autoptr (GBytes) bytes = NULL;
    record.version = 3;
    record.document = document;
    bytes = bro_archive_record_codec_encode_v3 (&record);
    if (!write_own (&run, output->record, bytes, error))
      return NULL;
  }
  _bro_step_log (c, BRO_MSG_ARCHIVE_RECORD_WRITTEN, _bro_param ("file", bro_json_value_new_string (output->record)), BRO_SEVERITY_INFO);

  /* An earlier attempt's own files that this one didn't write (a note, a log no longer there). */
  for (guint i = 0; i < tree->len; i++)
    {
      const char *f = tree->pdata[i];
      if (!strchr (f, '/') && bro_archive_files_is_own_file (f) && !was_written (&run, f))
        {
          g_autofree char *path = g_strconcat (staging, "/", f, NULL);
          if (!bro_file_system_remove (fs, path, error))
            return NULL;
        }
    }
  if (!bro_file_system_sync_directory (fs, staging, error))
    return NULL;

  g_free (output->status);
  output->status = g_strdup (read_errors > 0 ? "errors" : "success");
  g_ptr_array_unref (output->files);
  output->files = g_steal_pointer (&entries);
  g_ptr_array_unref (output->logs);
  output->logs = g_steal_pointer (&logs);
  output->note = g_steal_pointer (&note);
  output->tag = read_errors > 0 ? g_strdup (BRO_SEAL_STEP_READ_ERRORS_TAG) : NULL;
  return bro_step_report_new (bro_seal_output_encode (output));
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
bro_seal_step_class_init (BroSealStepClass *klass)
{
}

static void
bro_seal_step_init (BroSealStep *self)
{
}

BroSealStep *
bro_seal_step_new (void)
{
  return g_object_new (BRO_TYPE_SEAL_STEP, NULL);
}
