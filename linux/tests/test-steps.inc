/* test-steps.inc: shared/fixtures/engine/video-steps.cases.json, plan-steps.cases.json, acquire-steps.cases.json and
 * verify-rips.cases.json: each step alone, with fake ports. Included by test-engine.c (after test-pipeline.inc). */

#include "bro-acquire-step.h"
#include "bro-matroska.h"
#include "bro-mkv-track.h"
#include "bro-verify-rips-step.h"
#include "bro-directory-entry.h"
#include "bro-file-info.h"
#include "bro-file-tree.h"
#include "bro-await-media-step.h"
#include "bro-config-codec.h"
#include "bro-decide-step.h"
#include "bro-fingerprint.h"
#include "bro-identify-step.h"
#include "bro-plan-step.h"
#include "bro-probe-output.h"
#include "bro-unit-record.h"
#include "bro-volume-info.h"
#include "bro-listing-builder.h"
#include "bro-probe-step.h"
#include "bro-reconcile-step.h"
#include "bro-release-step.h"
#include "bro-robot.h"

#define STEPS_JS(v, k) bro_json_value_get_string (bro_json_value_member ((v), (k)), NULL)

/* ---- the ports a case gives, recording every call ---- */

#define TEST_TYPE_STEP_FAKES (test_step_fakes_get_type ())
G_DECLARE_FINAL_TYPE (TestStepFakes, test_step_fakes, TEST, STEP_FAKES, GObject)

struct _TestStepFakes {
  GObject parent;
  GMutex lock;
  BroJsonValue *given;
  GPtrArray *calls;    /* char * */
  GPtrArray *log;      /* BroBroMessage * */
  BroJsonValue *progress; /* array */
  char *fingerprint;   /* Probe's ('archivedBefore probe') */
  BroDecisionDto *question;
  GHashTable *files;   /* path → GBytes: what the step and the scripted tool made */
  GHashTable *folders; /* paths */
  GPtrArray *ops;      /* char *: the file system calls that change something, while recording */
  gboolean recording;
  guint next_run;      /* makemkv.runs taken */
  BroCancellationSource *cancel; /* the case's; not owned */
};

static void fakes_call (TestStepFakes *f, char *text);
static BroBroMessage *fakes_message (BroJsonValue *j, BroSeverity severity);

/* ---- files and folders in memory (and given.files, which exist as folders) ---- */

static void
steps_tree_record (TestStepFakes *f, char *op)
{
  if (f->recording)
    g_ptr_array_add (f->ops, op);
  else
    g_free (op);
}

/* The lock held. */
static void
steps_tree_put_folder (TestStepFakes *f, const char *path)
{
  g_auto (GStrv) parts = g_strsplit (path, "/", -1);
  GString *current = g_string_new ("");
  for (guint i = 0; parts[i]; i++)
    if (*parts[i])
      {
        g_string_append_printf (current, "/%s", parts[i]);
        g_hash_table_add (f->folders, g_strdup (current->str));
      }
  g_string_free (current, TRUE);
}

/* A file with these bytes (taken), its folders made; not recorded. */
static void
steps_tree_put (TestStepFakes *f, const char *path, GBytes *bytes)
{
  g_autofree char *folder = g_path_get_dirname (path);
  g_mutex_lock (&f->lock);
  steps_tree_put_folder (f, folder);
  g_hash_table_replace (f->files, g_strdup (path), bytes);
  g_mutex_unlock (&f->lock);
}

static void
steps_tree_put_dir (TestStepFakes *f, const char *path)
{
  g_mutex_lock (&f->lock);
  steps_tree_put_folder (f, path);
  g_mutex_unlock (&f->lock);
}

static BroBroError *
steps_tree_not_found (const char *path)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  return bro_bro_error_new ("fs.notFound", params, NULL);
}

static gboolean
steps_tree_under (const char *p, const char *folder)
{
  gsize n = strlen (folder);
  return strncmp (p, folder, n) == 0 && p[n] == '/';
}

static BroFileInfo *
steps_fakes_stat (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  BroFileInfo *info = NULL;
  GBytes *b;
  g_mutex_lock (&f->lock);
  if ((b = g_hash_table_lookup (f->files, path)))
    {
      info = bro_file_info_new ();
      info->size = g_bytes_get_size (b);
    }
  else if (g_hash_table_contains (f->folders, path))
    {
      info = bro_file_info_new ();
      info->is_directory = TRUE;
    }
  g_mutex_unlock (&f->lock);
  if (!info)
    *error = steps_tree_not_found (path);
  return info;
}

static int
steps_tree_by_name (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDirectoryEntry *const *) a)->name, (*(BroDirectoryEntry *const *) b)->name);
}

static GPtrArray *
steps_fakes_list (BroFileSystem *fs, const char *directory, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  GPtrArray *entries = NULL;
  GHashTableIter it;
  gpointer key;
  g_mutex_lock (&f->lock);
  if (g_hash_table_contains (f->folders, directory))
    {
      entries = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_directory_entry_free);
      for (int pass = 0; pass < 2; pass++)
        {
          g_hash_table_iter_init (&it, pass ? f->folders : f->files);
          while (g_hash_table_iter_next (&it, &key, NULL))
            {
              const char *p = key;
              if (steps_tree_under (p, directory) && !strchr (p + strlen (directory) + 1, '/'))
                {
                  BroDirectoryEntry *e = bro_directory_entry_new ();
                  e->name = g_strdup (p + strlen (directory) + 1);
                  e->is_directory = pass == 1;
                  g_ptr_array_add (entries, e);
                }
            }
        }
      g_ptr_array_sort (entries, steps_tree_by_name);
    }
  g_mutex_unlock (&f->lock);
  if (!entries)
    *error = steps_tree_not_found (directory);
  return entries;
}

static GBytes *
steps_fakes_read (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  GBytes *b;
  g_mutex_lock (&f->lock);
  b = g_hash_table_lookup (f->files, path);
  b = b ? g_bytes_ref (b) : NULL;
  g_mutex_unlock (&f->lock);
  if (!b)
    *error = steps_tree_not_found (path);
  return b;
}

static GBytes *
steps_fakes_read_range (BroFileSystem *fs, const char *path, gint64 offset, int length, BroBroError **error)
{
  g_autoptr (GBytes) all = steps_fakes_read (fs, path, error);
  gsize n;
  if (!all)
    return NULL;
  n = g_bytes_get_size (all);
  if ((gsize) offset >= n)
    return g_bytes_new (NULL, 0);
  return g_bytes_new_from_bytes (all, offset, MIN ((gsize) length, n - offset));
}

static gboolean
steps_fakes_create_directory (BroFileSystem *fs, const char *path, gboolean parents_must_exist, BroBroError **error)
{
  steps_tree_put_dir (TEST_STEP_FAKES (fs), path);
  return TRUE;
}

static gboolean
steps_fakes_write_atomically (BroFileSystem *fs, const char *path, GBytes *bytes, int mode, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  g_autofree char *folder = g_path_get_dirname (path);
  gboolean ok;
  g_mutex_lock (&f->lock);
  if ((ok = g_hash_table_contains (f->folders, folder)))
    {
      g_hash_table_replace (f->files, g_strdup (path), g_bytes_ref (bytes));
      steps_tree_record (f, g_strconcat ("write ", path, NULL));
    }
  g_mutex_unlock (&f->lock);
  if (!ok)
    *error = steps_tree_not_found (folder);
  return ok;
}

static gboolean
steps_fakes_rename (BroFileSystem *fs, const char *from, const char *to, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  gboolean ok = TRUE;
  GBytes *b;
  g_mutex_lock (&f->lock);
  if (g_hash_table_contains (f->files, to) || g_hash_table_contains (f->folders, to))
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (to));
      *error = bro_bro_error_new ("fs.alreadyExists", params, NULL);
      ok = FALSE;
    }
  else if ((b = g_hash_table_lookup (f->files, from)))
    {
      g_hash_table_replace (f->files, g_strdup (to), g_bytes_ref (b));
      g_hash_table_remove (f->files, from);
    }
  else if (g_hash_table_contains (f->folders, from))
    {
      g_autoptr (GPtrArray) moved = g_ptr_array_new_with_free_func (g_free);
      GHashTableIter it;
      gpointer key, value;
      g_hash_table_iter_init (&it, f->files);
      while (g_hash_table_iter_next (&it, &key, &value))
        if (steps_tree_under (key, from))
          g_ptr_array_add (moved, g_strdup (key));
      for (guint i = 0; i < moved->len; i++)
        {
          const char *old = moved->pdata[i];
          g_hash_table_replace (f->files, g_strconcat (to, old + strlen (from), NULL), g_bytes_ref (g_hash_table_lookup (f->files, old)));
          g_hash_table_remove (f->files, old);
        }
      g_ptr_array_set_size (moved, 0);
      g_hash_table_iter_init (&it, f->folders);
      while (g_hash_table_iter_next (&it, &key, NULL))
        if (strcmp (key, from) == 0 || steps_tree_under (key, from))
          g_ptr_array_add (moved, g_strdup (key));
      for (guint i = 0; i < moved->len; i++)
        {
          const char *old = moved->pdata[i];
          g_hash_table_add (f->folders, g_strconcat (to, old + strlen (from), NULL));
          g_hash_table_remove (f->folders, old);
        }
    }
  else
    {
      *error = steps_tree_not_found (from);
      ok = FALSE;
    }
  if (ok)
    steps_tree_record (f, g_strdup_printf ("rename %s %s", from, to));
  g_mutex_unlock (&f->lock);
  return ok;
}

static gboolean
steps_fakes_remove (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  gboolean ok = TRUE;
  g_mutex_lock (&f->lock);
  if (!g_hash_table_remove (f->files, path))
    {
      GHashTableIter it;
      gpointer key;
      gboolean empty = TRUE;
      if (!g_hash_table_contains (f->folders, path))
        {
          *error = steps_tree_not_found (path);
          ok = FALSE;
        }
      else
        {
          for (int pass = 0; pass < 2; pass++)
            {
              g_hash_table_iter_init (&it, pass ? f->folders : f->files);
              while (g_hash_table_iter_next (&it, &key, NULL))
                empty &= !steps_tree_under (key, path);
            }
          if (!empty)
            {
              BroJsonValue *params = bro_json_value_new_object ();
              bro_json_value_set (params, "path", bro_json_value_new_string (path));
              bro_json_value_set (params, "reason", bro_json_value_new_string ("not empty"));
              *error = bro_bro_error_new ("fs.failed", params, NULL);
              ok = FALSE;
            }
          else
            g_hash_table_remove (f->folders, path);
        }
    }
  if (ok)
    steps_tree_record (f, g_strconcat ("remove ", path, NULL));
  g_mutex_unlock (&f->lock);
  return ok;
}

static gboolean
steps_fakes_sync_directory (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  gboolean ok;
  g_mutex_lock (&f->lock);
  if ((ok = g_hash_table_contains (f->folders, path)))
    steps_tree_record (f, g_strconcat ("syncDirectory ", path, NULL));
  g_mutex_unlock (&f->lock);
  if (!ok)
    *error = steps_tree_not_found (path);
  return ok;
}

static gboolean
steps_fakes_sync_file (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  gboolean ok;
  g_mutex_lock (&f->lock);
  if ((ok = g_hash_table_contains (f->files, path)))
    steps_tree_record (f, g_strconcat ("sync ", path, NULL));
  g_mutex_unlock (&f->lock);
  if (!ok)
    *error = steps_tree_not_found (path);
  return ok;
}

/* ---- the scripted makemkvcon runs (makemkv.runs) ---- */

static BroJsonValue *
steps_fakes_next_run (TestStepFakes *f, BroBroError **error)
{
  BroJsonValue *runs = bro_json_value_member (bro_json_value_member (f->given, "makemkv"), "runs");
  BroJsonValue *run = NULL;
  g_mutex_lock (&f->lock);
  if (f->next_run < bro_json_value_length (runs))
    run = bro_json_value_at (runs, f->next_run++);
  g_mutex_unlock (&f->lock);
  if (!run)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "detail", bro_json_value_new_string ("no makemkv run left"));
      *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_INTERNAL_UNEXPECTED), params, NULL);
    }
  return run;
}

static BroMakemkvNotice *
steps_fakes_notice (const char *name)
{
  BroMakemkvNoticeKind kinds[] = { BRO_MAKEMKV_NOTICE_KEY_EXPIRED, BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED, BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD,
                                   BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED };
  const char *names[] = { "keyExpired", "evaluationNotStarted", "versionTooOld", "libreDriveRequired" };
  for (guint i = 0; name && i < G_N_ELEMENTS (names); i++)
    if (strcmp (name, names[i]) == 0)
      return bro_makemkv_notice_new (kinds[i], "");
  return NULL;
}

/* What a scripted run ends with (@produced: char *, taken). */
static BroMakemkvRun *
steps_fakes_ran (TestStepFakes *f, BroJsonValue *run, BroRunSink *sink, GPtrArray *produced)
{
  BroJsonValue *robot = bro_json_value_member (run, "robot");
  BroJsonValue *errors = bro_json_value_member (run, "readErrors");
  BroJsonValue *given_produced = bro_json_value_member (run, "produced");
  BroRunOutcome *outcome = g_new0 (BroRunOutcome, 1);
  BroMakemkvRun *made = bro_makemkv_run_new ();
  for (guint i = 0; i < bro_json_value_length (robot); i++)
    {
      g_autoptr (BroRobotEvent) e = bro_robot_parse_line (bro_json_value_get_string (bro_json_value_at (robot, i), ""));
      if (e)
        bro_run_sink_event (sink, e);
    }
  if (bro_json_value_get_bool (bro_json_value_member (run, "cancel"), FALSE))
    bro_cancellation_source_cancel (f->cancel);
  outcome->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (run, "exitCode"), 0);
  outcome->read_errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_robot_message_free);
  for (guint i = 0; i < bro_json_value_length (errors); i++)
    g_ptr_array_add (outcome->read_errors, bro_robot_message_new (2003, 0, bro_json_value_get_string (bro_json_value_at (errors, i), "")));
  if (given_produced)
    {
      g_ptr_array_unref (produced);
      produced = g_ptr_array_new_with_free_func (g_free);
      for (guint i = 0; i < bro_json_value_length (given_produced); i++)
        g_ptr_array_add (produced, g_strdup (bro_json_value_get_string (bro_json_value_at (given_produced, i), "")));
    }
  outcome->produced = produced;
  outcome->abandoned = bro_json_value_get_bool (bro_json_value_member (run, "abandoned"), FALSE);
  if (!bro_status_word_from_wire (bro_json_value_get_string (bro_json_value_member (run, "status"), "success"), &outcome->status))
    outcome->status = BRO_STATUS_WORD_SUCCESS;
  outcome->error = fakes_message (bro_json_value_member (run, "error"), BRO_SEVERITY_ERROR);
  made->outcome = outcome;
  made->notice = steps_fakes_notice (bro_json_value_get_string (bro_json_value_member (run, "notice"), NULL));
  made->transcript_problem = fakes_message (bro_json_value_member (run, "transcriptProblem"), BRO_SEVERITY_WARNING);
  made->settings_problem = fakes_message (bro_json_value_member (run, "settingsProblem"), BRO_SEVERITY_WARNING);
  made->isolation_warning = fakes_message (bro_json_value_member (run, "isolationWarning"), BRO_SEVERITY_WARNING);
  return made;
}

static char *
steps_fakes_describe (const BroMakemkvSource *source)
{
  return source->kind == BRO_MAKEMKV_SOURCE_DRIVE ? g_strdup_printf ("disc:%d", source->index)
         : source->kind == BRO_MAKEMKV_SOURCE_ISO ? g_strconcat ("iso:", source->path, NULL)
                                                  : g_strconcat ("file:", source->path, NULL);
}

static BroJsonValue *
_steps_tool (const char *tool)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "tool", bro_json_value_new_string (tool));
  return params;
}

static BroMakemkvRun *
steps_fakes_rip (BroMakemkv *makemkv, const BroMakemkvSource *source, const char *title, const char *destination,
           const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (makemkv);
  g_autofree char *described = steps_fakes_describe (source);
  BroJsonValue *run, *files;
  GPtrArray *produced;
  fakes_call (f, g_strdup_printf ("rip %s %s %s", described, title, destination));
  if (!(run = steps_fakes_next_run (f, error)))
    return NULL;
  files = bro_json_value_member (run, "files");
  produced = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < bro_json_value_length (files); i++)
    {
      const char *name = bro_json_value_get_string (bro_json_value_at (files, i), "");
      g_autofree char *path = g_strconcat (destination, "/", name, NULL);
      steps_tree_put (f, path, g_bytes_new (NULL, 0));
      g_ptr_array_add (produced, g_strdup (name));
    }
  return steps_fakes_ran (f, run, sink, produced);
}

static int
steps_tree_by_text (gconstpointer a, gconstpointer b)
{
  return strcmp (*(char *const *) a, *(char *const *) b);
}

static BroMakemkvRun *
steps_fakes_backup (BroMakemkv *makemkv, const BroMakemkvSource *source, gboolean decrypt, const char *destination,
              const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (makemkv);
  g_autofree char *described = steps_fakes_describe (source);
  BroJsonValue *run;
  GPtrArray *produced = g_ptr_array_new_with_free_func (g_free);
  fakes_call (f, g_strdup_printf ("backup %s %s %s", described, decrypt ? "true" : "false", destination));
  if (!(run = steps_fakes_next_run (f, error)))
    {
      g_ptr_array_unref (produced);
      return NULL;
    }
  if (bro_json_value_get_bool (bro_json_value_member (run, "image"), FALSE))
    {
      const char *header = bro_json_value_get_string (bro_json_value_member (run, "header"), "");
      GByteArray *bytes = g_byte_array_new ();
      g_byte_array_set_size (bytes, 32769);
      memset (bytes->data, 0, 32769);
      g_byte_array_append (bytes, (const guint8 *) header, strlen (header));
      steps_tree_put (f, destination, g_byte_array_free_to_bytes (bytes));
      g_ptr_array_add (produced, g_path_get_basename (destination));
    }
  else
    {
      BroJsonValue *files = bro_json_value_member (run, "files");
      steps_tree_put_dir (f, destination);
      for (guint i = 0; i < bro_json_value_length (files); i++)
        {
          const char *name = bro_json_value_get_string (bro_json_value_at (files, i), "");
          g_autofree char *path = g_strconcat (destination, "/", name, NULL);
          g_auto (GStrv) parts = g_strsplit (name, "/", 2);
          gboolean seen = FALSE;
          steps_tree_put (f, path, g_bytes_new (NULL, 0));
          for (guint j = 0; j < produced->len; j++)
            seen |= strcmp (produced->pdata[j], parts[0]) == 0;
          if (!seen)
            g_ptr_array_add (produced, g_strdup (parts[0]));
        }
      g_ptr_array_sort (produced, steps_tree_by_text);
    }
  return steps_fakes_ran (f, run, sink, produced);
}

/* ---- mkvmerge (given.matroska: a probe by file name, {duration, tracks, chapters} or null; missing; cancelled) ---- */

static BroMkvProbe *
steps_fakes_probe (BroMatroska *matroska, const char *file, BroCancellationToken *cancel, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (matroska);
  BroJsonValue *m = bro_json_value_member (f->given, "matroska"), *p, *tracks, *duration;
  const char *slash = strrchr (file, '/');
  BroMkvProbe *probe;
  fakes_call (f, g_strconcat ("probe ", file, NULL));
  if (bro_json_value_get_bool (bro_json_value_member (m, "missing"), FALSE))
    {
      *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_TOOL_MISSING), _steps_tool ("mkvmerge"), NULL);
      return NULL;
    }
  if (bro_json_value_get_bool (bro_json_value_member (m, "cancelled"), FALSE))
    {
      *error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL, NULL);
      return NULL;
    }
  p = bro_json_value_member (m, slash ? slash + 1 : file);
  if (!p || p->kind != BRO_JSON_VALUE_OBJECT)
    return NULL;
  probe = bro_mkv_probe_new ();
  duration = bro_json_value_member (p, "duration");
  if (duration && duration->kind != BRO_JSON_VALUE_NULL)
    {
      probe->has_duration = TRUE;
      probe->duration_seconds = bro_json_value_get_number (duration, 0);
    }
  tracks = bro_json_value_member (p, "tracks");
  for (guint i = 0; i < bro_json_value_length (tracks); i++)
    g_ptr_array_add (probe->tracks, bro_mkv_track_new ((int) i, bro_json_value_get_string (bro_json_value_at (tracks, i), "")));
  probe->chapter_count = (int) bro_json_value_get_integer (bro_json_value_member (p, "chapters"), 0);
  return probe;
}

static void steps_fakes_matroska_iface (BroMatroskaInterface *i) { i->probe = steps_fakes_probe; }

/* ---- the checkpoint (given.checkpoint; a fingerprint "probe" stands for Probe's, both ways) ---- */

static BroJsonValue *
steps_fakes_probed (TestStepFakes *f, const BroJsonValue *checkpoint, gboolean to_probe)
{
  BroJsonValue *o;
  if (checkpoint->kind != BRO_JSON_VALUE_OBJECT)
    return bro_json_value_ref ((BroJsonValue *) checkpoint);
  o = bro_json_value_new_object ();
  for (guint i = 0; i < checkpoint->keys->len; i++)
    {
      const char *key = checkpoint->keys->pdata[i];
      BroJsonValue *value = checkpoint->items->pdata[i];
      const char *text = bro_json_value_get_string (value, NULL);
      if (strcmp (key, "fingerprint") == 0 && text && (to_probe ? g_strcmp0 (text, f->fingerprint) == 0 : strcmp (text, "probe") == 0))
        bro_json_value_set (o, key, to_probe ? bro_json_value_new_string ("probe")
                                             : f->fingerprint ? bro_json_value_new_string (f->fingerprint) : bro_json_value_new_null ());
      else
        bro_json_value_set (o, key, bro_json_value_ref (value));
    }
  return o;
}

/* The canonical encoding on one line, as JobLog writes it. */
static char *
steps_one_line (const BroJsonValue *value)
{
  g_autofree char *canonical = bro_json_value_to_text (value);
  GString *out = g_string_new (NULL);
  for (const char *c = canonical; *c; c++)
    {
      if (*c != '\n')
        {
          g_string_append_c (out, *c);
          continue;
        }
      while (c[1] == ' ')
        c++;
    }
  return g_string_free (out, FALSE);
}

static BroJsonValue *
steps_fakes_saved (BroCheckpointer *checkpointer)
{
  TestStepFakes *f = TEST_STEP_FAKES (checkpointer);
  BroJsonValue *given = bro_json_value_member (f->given, "checkpoint");
  return given ? steps_fakes_probed (f, given, FALSE) : NULL;
}

static gboolean
steps_fakes_save (BroCheckpointer *checkpointer, const BroJsonValue *checkpoint, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (checkpointer);
  g_autoptr (BroJsonValue) probed = steps_fakes_probed (f, checkpoint, TRUE);
  g_autofree char *text = steps_one_line (probed);
  fakes_call (f, g_strconcat ("checkpoint ", text, NULL));
  return TRUE;
}

static void
fakes_call (TestStepFakes *f, char *text)
{
  g_mutex_lock (&f->lock);
  g_ptr_array_add (f->calls, text);
  g_mutex_unlock (&f->lock);
}

static BroBroMessage *
fakes_message (BroJsonValue *j, BroSeverity severity)
{
  BroMessageCode code;
  if (!j || !bro_message_code_parse (bro_json_value_get_string (bro_json_value_member (j, "code"), ""), &code))
    return NULL;
  BroJsonValue *params = bro_json_value_member (j, "params");
  return bro_bro_message_new (code, params ? bro_json_value_ref (params) : NULL, severity);
}

static BroDriveLocation *
fakes_find (BroDriveLookup *lookup, const char *drive_id)
{
  TestStepFakes *f = TEST_STEP_FAKES (lookup);
  BroJsonValue *d = bro_json_value_member (bro_json_value_member (f->given, "drives"), drive_id);
  if (!d)
    return NULL;
  BroDriveLocation *at = bro_drive_location_new (bro_json_value_get_string (bro_json_value_member (d, "device"), ""),
                                                 bro_json_value_get_string (bro_json_value_member (d, "configId"), NULL),
                                                 bro_json_value_get_string (bro_json_value_member (d, "label"), ""));
  BroJsonValue *v;
  if ((v = bro_json_value_member (d, "makemkvIndex")) && v->kind == BRO_JSON_VALUE_INTEGER) {
    at->has_makemkv_index = TRUE;
    at->makemkv_index = (int) v->integer;
  }
  if ((v = bro_json_value_member (d, "generation")) && v->kind == BRO_JSON_VALUE_INTEGER) {
    at->has_generation = TRUE;
    at->generation = v->integer;
  }
  if ((v = bro_json_value_member (d, "flags")) && v->kind == BRO_JSON_VALUE_INTEGER) {
    at->has_flags = TRUE;
    at->flags.raw = (int) v->integer;
  }
  return at;
}

static BroMakemkvInvocation *
fakes_invocation (BroMakemkvSetup *setup, const BroConfig *config, const BroEffectiveProfile *profile, const char *selection_override,
                  const char *job_directory, BroBroError **error)
{
  BroJsonValue *min = bro_json_value_member (bro_json_value_member (profile->profile, "makemkv"), "minLengthSeconds");
  g_autofree char *length = min && min->kind == BRO_JSON_VALUE_INTEGER ? g_strdup_printf ("%" G_GINT64_FORMAT, min->integer) : g_strdup ("null");
  fakes_call (TEST_STEP_FAKES (setup), g_strdup_printf ("invocation %s minLength=%s", selection_override ? selection_override : "-", length));
  return bro_makemkv_invocation_new ();
}

static BroListingRun *
fakes_listing (BroMakemkv *makemkv, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation, BroRunSink *sink,
               BroCancellationToken *cancel, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (makemkv);
  fakes_call (f, source->kind == BRO_MAKEMKV_SOURCE_DRIVE ? g_strdup_printf ("listing disc:%d", source->index)
                 : source->kind == BRO_MAKEMKV_SOURCE_ISO ? g_strconcat ("listing iso:", source->path, NULL)
                                                          : g_strconcat ("listing file:", source->path, NULL));
  BroJsonValue *m = bro_json_value_member (f->given, "makemkv"), *v;
  if (bro_json_value_member (m, "runs"))
    {
      BroJsonValue *run = steps_fakes_next_run (f, error);
      BroJsonValue *disc;
      g_autoptr (BroListingBuilder) listed = bro_listing_builder_new ();
      BroListingRun *read;
      if (!run)
        return NULL;
      if ((disc = bro_json_value_member (run, "listing")))
        {
          g_autofree char *robot = bro_test_generated_listing (disc);
          g_auto (GStrv) robot_lines = g_strsplit (robot, "\n", -1);
          for (guint i = 0; robot_lines[i]; i++)
            {
              g_autoptr (BroRobotEvent) e = bro_robot_parse_line (robot_lines[i]);
              if (e)
                {
                  bro_run_sink_event (sink, e);
                  bro_listing_builder_feed (listed, e);
                }
            }
        }
      read = bro_listing_run_new ();
      read->listing = bro_listing_builder_build (listed);
      read->run = steps_fakes_ran (f, run, sink, g_ptr_array_new_with_free_func (g_free));
      return read;
    }
  g_autofree char *text = NULL;
  if ((v = bro_json_value_member (m, "listing")))
    text = bro_test_generated_listing (v);
  else
    {
      GString *s = g_string_new (NULL);
      BroJsonValue *robot = bro_json_value_member (m, "robot");
      for (guint i = 0; i < bro_json_value_length (robot); i++)
        g_string_append_printf (s, "%s\n", bro_json_value_get_string (bro_json_value_at (robot, i), ""));
      text = g_string_free (s, FALSE);
    }
  g_autoptr (BroListingBuilder) builder = bro_listing_builder_new ();
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      g_autoptr (BroRobotEvent) e = bro_robot_parse_line (lines[i]);
      if (e)
        {
          bro_run_sink_event (sink, e);
          bro_listing_builder_feed (builder, e);
        }
    }
  BroJsonValue *o = bro_json_value_member (m, "outcome");
  BroRunOutcome *outcome = g_new0 (BroRunOutcome, 1);
  outcome->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (o, "exitCode"), 0);
  outcome->read_errors = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_robot_message_free);
  outcome->produced = g_ptr_array_new_with_free_func (g_free);
  if (!bro_status_word_from_wire (bro_json_value_get_string (bro_json_value_member (o, "status"), "success"), &outcome->status))
    outcome->status = BRO_STATUS_WORD_SUCCESS;
  outcome->error = fakes_message (bro_json_value_member (o, "error"), BRO_SEVERITY_ERROR);
  BroListingRun *read = bro_listing_run_new ();
  read->listing = bro_listing_builder_build (builder);
  read->run = bro_makemkv_run_new ();
  read->run->outcome = outcome;
  const char *notice = bro_json_value_get_string (bro_json_value_member (m, "notice"), "");
  BroMakemkvNoticeKind kinds[] = { BRO_MAKEMKV_NOTICE_KEY_EXPIRED, BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED, BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD,
                                   BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED };
  const char *names[] = { "keyExpired", "evaluationNotStarted", "versionTooOld", "libreDriveRequired" };
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (strcmp (notice, names[i]) == 0)
      read->run->notice = bro_makemkv_notice_new (kinds[i], "");
  read->run->libre_drive = g_strdup (bro_json_value_get_string (bro_json_value_member (m, "libreDrive"), NULL));
  read->run->transcript_problem = fakes_message (bro_json_value_member (m, "transcriptProblem"), BRO_SEVERITY_WARNING);
  read->run->settings_problem = fakes_message (bro_json_value_member (m, "settingsProblem"), BRO_SEVERITY_WARNING);
  read->run->isolation_warning = fakes_message (bro_json_value_member (m, "isolationWarning"), BRO_SEVERITY_WARNING);
  return read;
}

static gboolean
fakes_eject (BroDriveControl *drives, const char *device, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (drives);
  fakes_call (f, g_strconcat ("eject ", device, NULL));
  BroJsonValue *e = bro_json_value_member (f->given, "eject");
  if (!e)
    return TRUE;
  BroJsonValue *params = bro_json_value_member (e, "params");
  bro_bro_error_set (error, bro_json_value_get_string (bro_json_value_member (e, "code"), ""), params ? bro_json_value_ref (params) : NULL);
  return FALSE;
}

static char *
fakes_wait_for_mount (BroDriveControl *drives, const char *device, BroDuration timeout, BroCancellationToken *cancel)
{
  TestStepFakes *f = TEST_STEP_FAKES (drives);
  fakes_call (f, g_strdup_printf ("waitForMount %s %d", device, (int) timeout.seconds));
  return g_strdup (bro_json_value_get_string (bro_json_value_member (f->given, "mount"), NULL));
}

static void
fakes_report (BroProgressReporter *reporter, double fraction, const BroBroMessage *operation, const char *current)
{
  TestStepFakes *f = TEST_STEP_FAKES (reporter);
  BroJsonValue *item = bro_json_value_new_array ();
  bro_json_value_append (item, bro_json_value_new_number (fraction));
  bro_json_value_append (item, bro_json_value_new_string (bro_message_code_wire (operation->code)));
  if (current)
    bro_json_value_append (item, bro_json_value_new_string (current));
  g_mutex_lock (&f->lock);
  bro_json_value_append (f->progress, item);
  g_mutex_unlock (&f->lock);
}

static void
fakes_log (BroJobLogger *logger, const BroBroMessage *message)
{
  TestStepFakes *f = TEST_STEP_FAKES (logger);
  g_mutex_lock (&f->lock);
  g_ptr_array_add (f->log, bro_bro_message_copy (message));
  g_mutex_unlock (&f->lock);
}

static gboolean
steps_fakes_exists (BroFileSystem *fs, const char *path)
{
  TestStepFakes *f = TEST_STEP_FAKES (fs);
  BroJsonValue *files = bro_json_value_member (f->given, "files");
  gboolean found;
  for (guint i = 0; i < bro_json_value_length (files); i++)
    if (strcmp (bro_json_value_get_string (bro_json_value_at (files, i), ""), path) == 0)
      return TRUE;
  g_mutex_lock (&f->lock);
  found = g_hash_table_contains (f->files, path) || g_hash_table_contains (f->folders, path);
  g_mutex_unlock (&f->lock);
  return found;
}

static BroVolumeInfo *
fakes_volume (BroFileSystem *fs, const char *path, BroBroError **error)
{
  BroJsonValue *v = bro_json_value_member (bro_json_value_member (TEST_STEP_FAKES (fs)->given, "volumes"), path);
  BroVolumeInfo *info;
  if (!v)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (path));
      bro_bro_error_set (error, "fs.notFound", params);
      return NULL;
    }
  info = bro_volume_info_new ();
  g_free (info->id);
  info->id = g_strdup (STEPS_JS (v, "id"));
  info->free_bytes = bro_json_value_get_integer (bro_json_value_member (v, "free"), 0);
  return info;
}

static BroLibraryHandle *
fakes_require (BroLibraryPort *port, const char *library_id, BroBroError **error)
{
  BroJsonValue *l = bro_json_value_member (bro_json_value_member (TEST_STEP_FAKES (port)->given, "libraries"), library_id);
  BroJsonValue *why = bro_json_value_member (l, "offline");
  if (!l)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_LIBRARY_NOT_FOUND), NULL);
      return NULL;
    }
  if (why)
    {
      BroJsonValue *params = bro_json_value_member (why, "params");
      bro_bro_error_set (error, STEPS_JS (why, "code"), params ? bro_json_value_ref (params) : NULL);
      return NULL;
    }
  return bro_library_handle_new (library_id, STEPS_JS (l, "path"));
}

static char *
steps_fakes_staging_directory (BroLibraryPort *port, const char *library_id, const BroId *job_id, BroBroError **error)
{
  g_autoptr (BroLibraryHandle) library = fakes_require (port, library_id, error);
  char *path;
  if (!library)
    return NULL;
  path = g_strconcat (library->path, "/.bromelia/staging/", job_id->value, NULL);
  steps_tree_put_dir (TEST_STEP_FAKES (port), path);
  return path;
}

static GPtrArray *
fakes_archived_before (BroCatalogQueries *catalog, const char *fingerprint, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (catalog);
  GPtrArray *units = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_record_free);
  gboolean probe = g_strcmp0 (fingerprint, f->fingerprint) == 0;
  BroJsonValue *given = bro_json_value_member (f->given, "catalog");
  fakes_call (f, g_strconcat ("archivedBefore ", probe ? "probe" : fingerprint, NULL));
  for (guint i = 0; probe && i < bro_json_value_length (given); i++)
    {
      BroJsonValue *u = bro_json_value_at (given, i);
      BroUnitRecord *r = bro_unit_record_new ();
      g_strlcpy (r->id.value, STEPS_JS (u, "id"), sizeof r->id.value);
      g_free (r->library_id);
      r->library_id = g_strdup (STEPS_JS (u, "libraryId"));
      g_free (r->path);
      r->path = g_strdup (STEPS_JS (u, "path"));
      r->state = BRO_UNIT_STATE_COMMITTED;
      g_assert_true (bro_unit_status_from_wire (STEPS_JS (u, "status"), &r->status));
      g_assert_true (bro_instant_parse ("2026-09-01T00:00:00.000Z", &r->created_at));
      r->has_committed_at = bro_instant_parse (STEPS_JS (u, "committedAt"), &r->committed_at);
      g_ptr_array_add (units, r);
    }
  return units;
}

static char *
fakes_ask (BroDecisionPort *port, const BroDecisionDto *decision, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (port);
  GString *text = g_string_new (NULL);
  const char *answer = STEPS_JS (f->given, "answer");
  g_string_append_printf (text, "ask %s %s ", decision->id, bro_message_code_wire (decision->question->code));
  for (guint i = 0; i < decision->answers->len; i++)
    g_string_append_printf (text, "%s%s", i ? "," : "", ((BroDecisionAnswer *) g_ptr_array_index (decision->answers, i))->id);
  g_mutex_lock (&f->lock);
  bro_decision_dto_free (f->question);
  f->question = bro_decision_dto_copy (decision);
  g_mutex_unlock (&f->lock);
  fakes_call (f, g_string_free (text, FALSE));
  if (!answer)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return NULL;
    }
  return g_strdup (answer);
}

static gboolean
fakes_reserve (BroSpaceReservations *space, const char *volume, const char *path, gint64 need, gint64 free, BroBroError **error)
{
  TestStepFakes *f = TEST_STEP_FAKES (space);
  BroJsonValue *fails = bro_json_value_member (f->given, "reserveFails");
  fakes_call (f, g_strdup_printf ("reserve %s %s %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, volume, path, need, free));
  if (fails)
    {
      BroJsonValue *params = bro_json_value_member (fails, "params");
      bro_bro_error_set (error, STEPS_JS (fails, "code"), params ? bro_json_value_ref (params) : NULL);
      return FALSE;
    }
  return TRUE;
}

static void
steps_fakes_fs_iface (BroFileSystemInterface *i)
{
  i->exists = steps_fakes_exists;
  i->volume = fakes_volume;
  i->stat = steps_fakes_stat;
  i->list = steps_fakes_list;
  i->read = steps_fakes_read;
  i->read_range = steps_fakes_read_range;
  i->create_directory = steps_fakes_create_directory;
  i->write_atomically = steps_fakes_write_atomically;
  i->rename = steps_fakes_rename;
  i->remove = steps_fakes_remove;
  i->sync_file = steps_fakes_sync_file;
  i->sync_directory = steps_fakes_sync_directory;
}
static void
fakes_library_iface (BroLibraryPortInterface *i)
{
  i->require = fakes_require;
  i->staging_directory = steps_fakes_staging_directory;
}
static void
steps_fakes_checkpointer_iface (BroCheckpointerInterface *i)
{
  i->saved = steps_fakes_saved;
  i->save = steps_fakes_save;
}
static void fakes_catalog_iface (BroCatalogQueriesInterface *i) { i->archived_before = fakes_archived_before; }
static void fakes_decisions_iface (BroDecisionPortInterface *i) { i->ask = fakes_ask; }
static void fakes_space_iface (BroSpaceReservationsInterface *i) { i->reserve = fakes_reserve; }
static void fakes_lookup_iface (BroDriveLookupInterface *i) { i->find = fakes_find; }
static void fakes_setup_iface (BroMakemkvSetupInterface *i) { i->invocation = fakes_invocation; }
static void
fakes_makemkv_iface (BroMakemkvInterface *i)
{
  i->listing = fakes_listing;
  i->rip = steps_fakes_rip;
  i->backup = steps_fakes_backup;
}
static void
fakes_drives_iface (BroDriveControlInterface *i)
{
  i->eject = fakes_eject;
  i->wait_for_mount = fakes_wait_for_mount;
}
static void fakes_progress_iface (BroProgressReporterInterface *i) { i->report = fakes_report; }
static void fakes_log_iface (BroJobLoggerInterface *i) { i->log = fakes_log; }

G_DEFINE_FINAL_TYPE_WITH_CODE (TestStepFakes, test_step_fakes, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_LOOKUP, fakes_lookup_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MAKEMKV_SETUP, fakes_setup_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MAKEMKV, fakes_makemkv_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_CONTROL, fakes_drives_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_PROGRESS_REPORTER, fakes_progress_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_JOB_LOGGER, fakes_log_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, steps_fakes_fs_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_LIBRARY_PORT, fakes_library_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CATALOG_QUERIES, fakes_catalog_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DECISION_PORT, fakes_decisions_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_SPACE_RESERVATIONS, fakes_space_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CHECKPOINTER, steps_fakes_checkpointer_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MATROSKA, steps_fakes_matroska_iface))

static void
test_step_fakes_finalize (GObject *object)
{
  TestStepFakes *f = TEST_STEP_FAKES (object);
  bro_json_value_unref (f->given);
  g_ptr_array_unref (f->calls);
  g_ptr_array_unref (f->log);
  bro_json_value_unref (f->progress);
  g_free (f->fingerprint);
  bro_decision_dto_free (f->question);
  g_hash_table_unref (f->files);
  g_hash_table_unref (f->folders);
  g_ptr_array_unref (f->ops);
  g_mutex_clear (&f->lock);
  G_OBJECT_CLASS (test_step_fakes_parent_class)->finalize (object);
}

static void
test_step_fakes_class_init (TestStepFakesClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_step_fakes_finalize;
}

static void
test_step_fakes_init (TestStepFakes *f)
{
  g_mutex_init (&f->lock);
  f->calls = g_ptr_array_new_with_free_func (g_free);
  f->log = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  f->progress = bro_json_value_new_array ();
  f->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
  f->folders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add (f->folders, g_strdup ("/"));
  f->ops = g_ptr_array_new_with_free_func (g_free);
}

/* ---- the cases ---- */

/* Every key of @expected is in @actual with the same value; arrays have the same length and match element by element;
 * numbers compare as numbers. */
static void
steps_subset (GPtrArray *failures, const char *id, const char *path, const BroJsonValue *expected, const BroJsonValue *actual)
{
  if (expected->kind == BRO_JSON_VALUE_OBJECT)
    {
      if (!actual || actual->kind != BRO_JSON_VALUE_OBJECT)
        {
          bro_test_fail (failures, id, "%s: expected an object", path);
          return;
        }
      for (guint i = 0; i < expected->keys->len; i++)
        {
          g_autofree char *p = g_strconcat (path, ".", (char *) expected->keys->pdata[i], NULL);
          BroJsonValue *a = bro_json_value_member (actual, expected->keys->pdata[i]);
          if (!a)
            bro_test_fail (failures, id, "%s: missing", p);
          else
            steps_subset (failures, id, p, expected->items->pdata[i], a);
        }
    }
  else if (expected->kind == BRO_JSON_VALUE_ARRAY)
    {
      if (!actual || actual->kind != BRO_JSON_VALUE_ARRAY || actual->items->len != expected->items->len)
        {
          bro_test_same_json (failures, id, path, expected, actual);
          return;
        }
      for (guint i = 0; i < expected->items->len; i++)
        {
          g_autofree char *p = g_strdup_printf ("%s[%u]", path, i);
          steps_subset (failures, id, p, expected->items->pdata[i], actual->items->pdata[i]);
        }
    }
  else if ((expected->kind == BRO_JSON_VALUE_INTEGER || expected->kind == BRO_JSON_VALUE_NUMBER) && actual
           && (actual->kind == BRO_JSON_VALUE_INTEGER || actual->kind == BRO_JSON_VALUE_NUMBER))
    {
      if (bro_json_value_get_number (expected, 0) != bro_json_value_get_number (actual, 0))
        bro_test_same_json (failures, id, path, expected, actual);
    }
  else
    bro_test_same_json (failures, id, path, expected, actual);
}

static BroJobRequest *
steps_request_of (const BroJsonValue *r)
{
  BroJobKind kind;
  if (!bro_job_kind_from_wire (bro_json_value_get_string (bro_json_value_member (r, "kind"), ""), &kind))
    kind = BRO_JOB_KIND_VIDEO_DISC;
  BroJobRequest *q = bro_job_request_new (kind, bro_json_value_get_string (bro_json_value_member (r, "title"), ""));
  q->automatic = bro_json_value_get_bool (bro_json_value_member (r, "automatic"), FALSE);
  q->drive_id = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "driveId"), NULL));
  BroJsonValue *v;
  if ((v = bro_json_value_member (r, "mediaGeneration")) && v->kind == BRO_JSON_VALUE_INTEGER)
    {
      q->has_media_generation = TRUE;
      q->media_generation = v->integer;
    }
  if ((v = bro_json_value_member (r, "sessionId")) && v->kind == BRO_JSON_VALUE_STRING)
    {
      q->session_id = g_new0 (BroId, 1);
      g_strlcpy (q->session_id->value, v->string, sizeof q->session_id->value);
    }
  if ((v = bro_json_value_member (r, "options")))
    q->options = bro_json_value_ref (v);
  if ((v = bro_json_value_member (r, "details")))
    q->details = bro_json_value_ref (v);
  return q;
}

static BroStep *
steps_step (const char *kind)
{
  if (strcmp (kind, "awaitMedia") == 0)
    return BRO_STEP (bro_await_media_step_new ());
  if (strcmp (kind, "probe") == 0)
    return BRO_STEP (bro_probe_step_new ());
  if (strcmp (kind, "reconcile") == 0)
    return BRO_STEP (bro_reconcile_step_new ());
  if (strcmp (kind, "identify") == 0)
    return BRO_STEP (bro_identify_step_new ());
  if (strcmp (kind, "plan") == 0)
    return BRO_STEP (bro_plan_step_new ());
  if (strcmp (kind, "decide") == 0)
    return BRO_STEP (bro_decide_step_new ());
  if (strcmp (kind, "acquire") == 0)
    return BRO_STEP (bro_acquire_step_new ());
  if (strcmp (kind, "verifyRips") == 0)
    return BRO_STEP (bro_verify_rips_step_new ());
  if (strcmp (kind, "release") == 0)
    return BRO_STEP (bro_release_step_new ());
  return NULL;
}

static const char *
steps_notice_name (const BroMakemkvNotice *n)
{
  if (!n)
    return NULL;
  switch (n->kind)
    {
    case BRO_MAKEMKV_NOTICE_KEY_EXPIRED: return "keyExpired";
    case BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED: return "evaluationNotStarted";
    case BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD: return "versionTooOld";
    case BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED: return "libreDriveRequired";
    case BRO_MAKEMKV_NOTICE_LIBRE_DRIVE: return "libreDrive";
    }
  return NULL;
}

static gboolean
video_steps_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroStep) step = steps_step (bro_json_value_get_string (bro_json_value_member (given, "step"), ""));
  if (!step)
    return FALSE;
  TestStepFakes *fakes = g_object_new (TEST_TYPE_STEP_FAKES, NULL);
  fakes->given = bro_json_value_ref (given);
  BroJsonValue *config_json = bro_json_value_member (given, "config"), *v;
  g_autoptr (BroJsonValue) default_config = bro_json_value_new_object ();
  bro_json_value_set (default_config, "version", bro_json_value_new_integer (3));
  g_autofree char *config_text = bro_json_value_to_text (config_json ? config_json : default_config);
  g_autoptr (BroConfig) config = bro_config_codec_decode (config_text, -1, NULL);
  BroId job_id = { "00000000-0000-4000-8000-000000000001" };
  BroJobSnapshot *job = bro_job_snapshot_new (&job_id, steps_request_of (bro_json_value_member (given, "request")), NULL);
  BroJsonValue *outputs = bro_json_value_member (given, "outputs");
  BroJsonValue *probe = bro_json_value_member (given, "probe");
  g_autofree char *staging = NULL;
  g_autoptr (BroListing) probed_listing = NULL;
  if (probe)
    {
      BroJsonValue *disc = bro_json_value_member (probe, "disc");
      g_autoptr (BroListing) listing = NULL;
      g_autoptr (BroProbeOutput) output = NULL;
      if (disc)
        {
          g_autofree char *robot = bro_test_generated_listing (disc);
          g_auto (GStrv) lines = g_strsplit (robot, "\n", -1);
          g_autoptr (BroListingBuilder) builder = bro_listing_builder_new ();
          for (guint i = 0; lines[i]; i++)
            {
              g_autoptr (BroRobotEvent) e = bro_robot_parse_line (lines[i]);
              if (e)
                bro_listing_builder_feed (builder, e);
            }
          listing = bro_listing_builder_build (builder);
          fakes->fingerprint = bro_fingerprint_of (listing);
          probed_listing = bro_listing_ref (listing);
        }
      output = bro_probe_output_new (listing, fakes->fingerprint, NULL, FALSE);
      g_ptr_array_add (job->outputs, bro_step_output_new (BRO_STEP_KIND_PROBE, bro_probe_output_encode (output), NULL));
    }
  for (guint i = 0; i < bro_json_value_length (outputs); i++)
    {
      BroJsonValue *o = bro_json_value_at (outputs, i);
      BroStepKind kind;
      g_assert_true (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_member (o, "kind"), ""), &kind));
      g_ptr_array_add (job->outputs, bro_step_output_new (kind, bro_json_value_ref (bro_json_value_member (o, "data")), NULL));
      ((BroStepOutput *) job->outputs->pdata[job->outputs->len - 1])->read_errors =
        (int) bro_json_value_get_integer (bro_json_value_member (o, "readErrors"), 0);
    }
  {
    BroJsonValue *libraries = bro_json_value_member (given, "libraries");
    BroJsonValue *tree = bro_json_value_member (given, "tree");
    const char *root = bro_json_value_get_string (bro_json_value_member (bro_json_value_member (libraries, "lib"), "path"), "/lib");
    staging = g_strconcat (root, "/.bromelia/staging/", job_id.value, NULL);
    for (guint i = 0; i < bro_json_value_length (tree); i++)
      {
        const char *entry = bro_json_value_get_string (bro_json_value_at (tree, i), "");
        g_autofree char *path = g_strconcat (staging, "/", entry, NULL);
        if (g_str_has_suffix (path, "/"))
          {
            path[strlen (path) - 1] = 0;
            steps_tree_put_dir (fakes, path);
          }
        else
          steps_tree_put (fakes, path, g_bytes_new (NULL, 0));
      }
    for (guint i = 0; (tree = bro_json_value_member (given, "images")) && i < tree->keys->len; i++)
      {
        const char *header = bro_json_value_get_string (tree->items->pdata[i], "");
        g_autofree char *path = g_strconcat (staging, "/", (char *) tree->keys->pdata[i], NULL);
        GByteArray *bytes = g_byte_array_new ();
        g_byte_array_set_size (bytes, 32769);
        memset (bytes->data, 0, 32769);
        g_byte_array_append (bytes, (const guint8 *) header, strlen (header));
        steps_tree_put (fakes, path, g_byte_array_free_to_bytes (bytes));
      }
  }
  g_autoptr (BroToolSet) tools = bro_tool_set_new (BRO_MAKEMKV (fakes), BRO_MATROSKA (fakes));
  g_autoptr (BroStepEnvironment) environment = bro_step_environment_new (tools, BRO_FILE_SYSTEM (fakes), NULL, BRO_DRIVE_CONTROL (fakes),
                                                                         BRO_LIBRARY_PORT (fakes), BRO_CATALOG_QUERIES (fakes), NULL,
                                                                         BRO_DRIVE_LOOKUP (fakes), BRO_MAKEMKV_SETUP (fakes), "/data");
  g_autoptr (BroCancellationSource) cancel = bro_cancellation_source_new ();
  g_autofree char *directory = g_strconcat ("/data/jobs/", job_id.value, NULL);
  fakes->cancel = cancel;
  if (bro_json_value_get_bool (bro_json_value_member (given, "identify"), FALSE))
    {
      BroJobSnapshot *before = bro_job_snapshot_new (&job_id, bro_job_request_copy (job->request), NULL);
      g_autoptr (BroStep) identify = BRO_STEP (bro_identify_step_new ());
      g_autoptr (BroBroError) failure = NULL;
      for (guint i = 0; i < job->outputs->len; i++)
        g_ptr_array_add (before->outputs, bro_step_output_copy (g_ptr_array_index (job->outputs, i)));
      g_autoptr (BroStepContext) first = bro_step_context_new (before, bro_cancellation_source_token (cancel), g_object_ref (BRO_PROGRESS_REPORTER (fakes)),
                                                               g_object_ref (BRO_JOB_LOGGER (fakes)), g_object_ref (BRO_DECISION_PORT (fakes)),
                                                               g_object_ref (BRO_SPACE_RESERVATIONS (fakes)), g_object_ref (BRO_CHECKPOINTER (fakes)),
                                                               environment, config, directory);
      g_autoptr (BroStepReport) identified = bro_step_run (identify, first, &failure);
      g_assert_nonnull (identified);
      g_ptr_array_add (job->outputs, bro_step_output_new (BRO_STEP_KIND_IDENTIFY, bro_json_value_ref (identified->data), NULL));
      g_ptr_array_set_size (fakes->calls, 0);
      g_ptr_array_set_size (fakes->log, 0);
      bro_json_value_unref (fakes->progress);
      fakes->progress = bro_json_value_new_array ();
    }
  g_autoptr (BroResourceSet) resources = bro_step_resources (step, job);
  g_autoptr (BroStepContext) context = bro_step_context_new (job, bro_cancellation_source_token (cancel), g_object_ref (BRO_PROGRESS_REPORTER (fakes)),
                                                             g_object_ref (BRO_JOB_LOGGER (fakes)), g_object_ref (BRO_DECISION_PORT (fakes)),
                                                             g_object_ref (BRO_SPACE_RESERVATIONS (fakes)), g_object_ref (BRO_CHECKPOINTER (fakes)),
                                                             environment, config, directory);
  if ((v = bro_json_value_member (given, "finishing")))
    {
      BroJsonValue *why = bro_json_value_member (v, "error");
      context->finishing = g_new0 (BroDecided, 1);
      g_assert_true (bro_outcome_from_wire (STEPS_JS (v, "outcome"), &context->finishing->outcome));
      context->finishing->error = why ? bro_bro_error_new (STEPS_JS (why, "code"), NULL, NULL) : NULL;
    }

  if ((v = bro_json_value_member (expect, "resources")))
    {
      g_autoptr (BroJsonValue) drives = bro_json_value_new_array ();
      for (guint i = 0; resources->drives && resources->drives[i]; i++)
        bro_json_value_append (drives, bro_json_value_new_string (resources->drives[i]));
      bro_test_same_json (failures, id, "resources.drives", bro_json_value_member (v, "drives"), drives);
      if (bro_json_value_member (v, "libraryWrites"))
        {
          g_autoptr (BroJsonValue) writes = bro_json_value_new_array ();
          for (guint i = 0; resources->library_writes && resources->library_writes[i]; i++)
            bro_json_value_append (writes, bro_json_value_new_string (resources->library_writes[i]));
          bro_test_same_json (failures, id, "resources.libraryWrites", bro_json_value_member (v, "libraryWrites"), writes);
        }
      if (bro_json_value_member (v, "acquisition")
          && bro_json_value_get_bool (bro_json_value_member (v, "acquisition"), FALSE) != resources->acquisition)
        bro_test_fail (failures, id, "resources.acquisition: expected %s", resources->acquisition ? "false" : "true");
    }
  if ((v = bro_json_value_member (expect, "resumes")))
    {
      bro_test_same_string (failures, id, "resumability", STEPS_JS (v, "resumability"), bro_resumability_to_wire (bro_step_resumability (step)));
      bro_test_same_string (failures, id, "resumeCheckpoint", STEPS_JS (v, "checkpoint"), bro_step_resume_checkpoint (step));
    }
  g_autoptr (BroBroError) thrown = NULL;
  fakes->recording = TRUE;
  g_autoptr (BroStepReport) report = bro_step_run (step, context, &thrown);
  fakes->recording = FALSE;
  const BroBroError *error = thrown ? thrown : report ? report->error : NULL;
  if ((v = bro_json_value_member (expect, "error")))
    {
      bro_test_same_string (failures, id, "error", STEPS_JS (v, "code"), error ? error->code : NULL);
      g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
      BroJsonValue *params = bro_json_value_member (v, "params");
      bro_test_same_json (failures, id, "error params", params ? params : none, error ? error->params : none);
    }
  else if (error)
    bro_test_fail (failures, id, "unexpected error %s", error->code);
  if ((v = bro_json_value_member (expect, "notice")))
    bro_test_same_string (failures, id, "notice", v->string, steps_notice_name (report ? report->notice : NULL));
  if (bro_json_value_get_bool (bro_json_value_member (expect, "skipped"), FALSE) != (report && report->skipped))
    bro_test_fail (failures, id, "skipped: expected %s", bro_json_value_get_bool (bro_json_value_member (expect, "skipped"), FALSE) ? "true" : "false");
  if ((v = bro_json_value_member (expect, "data")))
    steps_subset (failures, id, "data", v, report ? report->data : NULL);
  if ((v = bro_json_value_member (expect, "log")))
    {
      GString *codes = g_string_new (NULL);
      for (guint i = 0; i < fakes->log->len; i++)
        g_string_append_printf (codes, "%s%s", i ? ", " : "", bro_message_code_wire (((BroBroMessage *) fakes->log->pdata[i])->code));
      if (bro_json_value_length (v) != fakes->log->len)
        bro_test_fail (failures, id, "log: expected %u lines, got %u (%s)", bro_json_value_length (v), fakes->log->len, codes->str);
      else
        for (guint i = 0; i < fakes->log->len; i++)
          {
            BroJsonValue *e = bro_json_value_at (v, i);
            const BroBroMessage *got = fakes->log->pdata[i];
            g_autofree char *what = g_strdup_printf ("log[%u]", i);
            bro_test_same_string (failures, id, what, e->kind == BRO_JSON_VALUE_STRING ? e->string : STEPS_JS (e, "code"),
                                  bro_message_code_wire (got->code));
            if (e->kind == BRO_JSON_VALUE_OBJECT)
              {
                g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
                BroJsonValue *params = bro_json_value_member (e, "params");
                g_autofree char *what_params = g_strdup_printf ("log[%u] params", i);
                bro_test_same_json (failures, id, what_params, params ? params : none, got->params);
                if (STEPS_JS (e, "severity"))
                  bro_test_same_string (failures, id, what, STEPS_JS (e, "severity"), bro_severity_to_wire (got->severity));
              }
          }
      g_string_free (codes, TRUE);
    }
  if ((v = bro_json_value_member (expect, "calls")))
    {
      GString *want = g_string_new (NULL), *got = g_string_new (NULL);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_string_append_printf (want, "%s%s", i ? " | " : "", bro_json_value_get_string (bro_json_value_at (v, i), ""));
      for (guint i = 0; i < fakes->calls->len; i++)
        g_string_append_printf (got, "%s%s", i ? " | " : "", (char *) fakes->calls->pdata[i]);
      bro_test_same_string (failures, id, "calls", want->str, got->str);
      g_string_free (want, TRUE);
      g_string_free (got, TRUE);
    }
  if ((v = bro_json_value_member (expect, "progress")))
    steps_subset (failures, id, "progress", v, fakes->progress);
  if ((v = bro_json_value_member (expect, "skipJob")))
    {
      const BroBroMessage *skip = report ? report->skip_job : NULL;
      bro_test_same_string (failures, id, "skipJob", v->kind == BRO_JSON_VALUE_NULL ? NULL : STEPS_JS (v, "code"),
                            skip ? bro_message_code_wire (skip->code) : NULL);
      if (bro_json_value_member (v, "params"))
        {
          g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
          bro_test_same_json (failures, id, "skipJob params", bro_json_value_member (v, "params"), skip ? skip->params : none);
        }
    }
  if ((v = bro_json_value_member (expect, "appendSteps")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      for (guint i = 0; report && i < report->append_steps->len; i++)
        bro_json_value_append (got, bro_json_value_new_string (bro_step_kind_to_wire (g_array_index (report->append_steps, BroStepKind, i))));
      bro_test_same_json (failures, id, "appendSteps", v, got);
    }
  if ((v = bro_json_value_member (expect, "plan")))
    {
      g_autoptr (BroJsonValue) got = NULL;
      if (report && report->plan)
        {
          got = bro_json_value_new_object ();
          bro_json_value_set (got, "mode", report->plan->mode ? bro_json_value_new_string (report->plan->mode) : bro_json_value_new_null ());
          bro_json_value_set (got, "profileId", report->plan->profile_id ? bro_json_value_new_string (report->plan->profile_id) : bro_json_value_new_null ());
          bro_json_value_set (got, "libraryId", report->plan->library_id ? bro_json_value_new_string (report->plan->library_id) : bro_json_value_new_null ());
        }
      steps_subset (failures, id, "plan", v, got);
    }
  if ((v = bro_json_value_member (expect, "readErrors")) && v->integer != (report ? report->read_errors : 0))
    bro_test_fail (failures, id, "readErrors: expected %" G_GINT64_FORMAT ", got %d", v->integer, report ? report->read_errors : 0);
  if ((v = bro_json_value_member (expect, "tree")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      g_autoptr (BroBroError) unlisted = NULL;
      g_autoptr (GPtrArray) files = bro_file_system_exists (BRO_FILE_SYSTEM (fakes), staging)
                                        ? bro_file_tree_files (BRO_FILE_SYSTEM (fakes), staging, &unlisted)
                                        : g_ptr_array_new ();
      for (guint i = 0; files && i < files->len; i++)
        bro_json_value_append (got, bro_json_value_new_string (files->pdata[i]));
      bro_test_same_json (failures, id, "tree", v, got);
    }
  if ((v = bro_json_value_member (expect, "fs")))
    {
      GString *want = g_string_new (NULL), *got = g_string_new (NULL);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_string_append_printf (want, "%s%s", i ? " | " : "", bro_json_value_get_string (bro_json_value_at (v, i), ""));
      for (guint i = 0; i < fakes->ops->len; i++)
        g_string_append_printf (got, "%s%s", i ? " | " : "", (char *) fakes->ops->pdata[i]);
      bro_test_same_string (failures, id, "fs", want->str, got->str);
      g_string_free (want, TRUE);
      g_string_free (got, TRUE);
    }
  if ((v = bro_json_value_member (expect, "json")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        g_autofree char *path = g_strconcat (staging, "/", (char *) v->keys->pdata[i], NULL);
        g_autoptr (BroBroError) unread = NULL;
        g_autoptr (GBytes) bytes = steps_fakes_read (BRO_FILE_SYSTEM (fakes), path, &unread);
        g_autoptr (BroJsonValue) got = bytes ? bro_json_value_parse (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes)) : NULL;
        g_autoptr (BroJsonValue) want = g_strcmp0 (bro_json_value_get_string (v->items->pdata[i], NULL), "probe") == 0 && probed_listing
                                            ? bro_listing_encode (probed_listing)
                                            : bro_json_value_ref (v->items->pdata[i]);
        g_autofree char *what = g_strconcat ("json ", (char *) v->keys->pdata[i], NULL);
        bro_test_same_json (failures, id, what, want, got);
      }
  if ((v = bro_json_value_member (expect, "question")))
    {
      g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
      BroJsonValue *params = bro_json_value_member (v, "params");
      bro_test_same_string (failures, id, "question", STEPS_JS (v, "code"), fakes->question ? bro_message_code_wire (fakes->question->question->code) : NULL);
      bro_test_same_json (failures, id, "question params", params ? params : none, fakes->question ? fakes->question->question->params : none);
    }
  g_clear_pointer (&context, bro_step_context_free);
  g_object_unref (fakes);
  return TRUE;
}

static void
test_video_steps (void)
{
  bro_test_run_cases ("engine/video-steps.cases.json", video_steps_case);
}

static void
test_plan_steps (void)
{
  bro_test_run_cases ("engine/plan-steps.cases.json", video_steps_case);
}

static void
test_acquire_steps (void)
{
  bro_test_run_cases ("engine/acquire-steps.cases.json", video_steps_case);
}

static void
test_verify_rips (void)
{
  bro_test_run_cases ("engine/verify-rips.cases.json", video_steps_case);
}
