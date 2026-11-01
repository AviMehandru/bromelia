/* test-scenarios.c: the scenario test unit (plan §5.1): shared/fixtures/engine/local-engine.cases.json, LocalEngine over
 * TestCompositionRoot. The harness's steps are the C# LocalEngineTests' one for one (the case file's description). A
 * case that goes wrong (a call that never ends, an engine that never settles) is a case failure, never an abort, and
 * the run stops like the engine: whatever is still going is stopped and settled before the next case. */
#include "bro-test-composition-root.h"
#include "bro-test-fixtures.h"

#include "bro-config-codec.h"
#include "bro-drive-dto.h"
#include "bro-issue.h"
#include "bro-log-line-dto.h"
#include "bro-message-code.h"
#include "bro-scheduler-limits.h"

#include <string.h>

/* How long a call, a feed read or a settle may take. */
#define PATIENCE 10.0
/* How long a stop or a restart may take (the engine's own cap is 30 s). */
#define STOP_PATIENCE 40.0
#define SEEDED "00000000-0000-4000-8000-1000000000"
#define NOBODY "00000000-0000-4000-8000-0000000000ff"
#define MARKER "/.bromelia-library"

static const char *const default_scan[] = {
  "DRV:0,2,999,12,\"BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325\",\"MOVIE_DISC\",\"/dev/rdisk4\"",
  "DRV:1,0,999,0,\"DVD+R-DL MATSHITA DVD-R   UJ8C2 SB00\",\"\",\"/dev/rdisk6\"",
  NULL,
};

typedef struct {
  TestCompositionRoot *r;
  GHashTable *jobs;  /* name → job id */
  GHashTable *feeds; /* name → BroEventFeed */
  GHashTable *seqs;  /* name → its seq as text */
  GStrv scan;
  GStrv listing;
  GHashTable *holds; /* the makemkvcon commands that run until stopped */
  gint slow_scan;    /* 1 once the first drive scan was slowed (or none is) */
  GMutex lock;       /* released, under it */
  gboolean released;
  gboolean released_executor_stopped;
  int released_running;
  TestWaiter *stopping;
  char *failure; /* the step that failed and why */
} Run;

#define J(v, k) bro_json_value_member ((v), (k))
#define S(v, k, d) bro_json_value_get_string (J ((v), (k)), (d))

static void
step_failed (Run *run, const char *format, ...) G_GNUC_PRINTF (2, 3);

static void
step_failed (Run *run, const char *format, ...)
{
  va_list args;
  if (run->failure)
    return;
  va_start (args, format);
  run->failure = g_strdup_vprintf (format, args);
  va_end (args);
}

/* The JSON on one line (failure messages). */
static char *
one_line (const BroJsonValue *v)
{
  return v ? bro_json_value_to_text (v) : g_strdup ("null");
}

/* ---- the world -------------------------------------------------------------------------------------------------- */

static gboolean
has_argument (const BroProcessSpec *spec, const char *argument)
{
  for (guint i = 0; spec->arguments && spec->arguments[i]; i++)
    if (strcmp (spec->arguments[i], argument) == 0)
      return TRUE;
  return FALSE;
}

/* The makemkvcon command of a run: info, mkv or backup (NULL: none). */
static const char *
verb (const BroProcessSpec *spec)
{
  for (guint i = 0; spec->arguments && spec->arguments[i]; i++)
    if (!strcmp (spec->arguments[i], "info") || !strcmp (spec->arguments[i], "mkv") || !strcmp (spec->arguments[i], "backup"))
      return spec->arguments[i];
  return NULL;
}

static GStrv
lines_for (const BroProcessSpec *spec, gpointer data)
{
  Run *run = data;
  const char *v = verb (spec);
  if (has_argument (spec, "disc:9999"))
    {
      if (g_atomic_int_compare_and_exchange (&run->slow_scan, 0, 1))
        g_usleep (300 * 1000);
      return g_strdupv (run->scan);
    }
  if (v && strcmp (v, "info") == 0)
    return g_strdupv (run->listing);
  return g_new0 (char *, 1);
}

static gboolean
runs_until_stopped (const BroProcessSpec *spec, gpointer data)
{
  Run *run = data;
  const char *v = verb (spec);
  return !has_argument (spec, "disc:9999") && v && g_hash_table_contains (run->holds, v);
}

static void
slow_marker (const char *path, gpointer data)
{
  if (g_str_has_suffix (path, MARKER))
    g_usleep (300 * 1000); /* recovery reads it too, before libraryService.start */
}

static void
nothing (gpointer data)
{
}

static void
released (gpointer data)
{
  Run *run = data;
  g_mutex_lock (&run->lock);
  run->released = TRUE;
  run->released_executor_stopped = !bro_engine_executor_post (run->r->executor, nothing, NULL, NULL);
  run->released_running = test_scripted_launcher_running (run->r->launcher);
  g_mutex_unlock (&run->lock);
}

static BroBroError *
fs_failed (const char *op, const char *path)
{
  BroJsonValue *p = bro_json_value_new_object ();
  bro_json_value_set (p, "operation", bro_json_value_new_string (op));
  bro_json_value_set (p, "path", bro_json_value_new_string (path));
  bro_json_value_set (p, "reason", bro_json_value_new_string ("injected"));
  return bro_bro_error_new ("fs.failed", p, NULL);
}

static char *
marker_text (const char *id, const char *name)
{
  return g_strdup_printf ("{\n  \"id\": \"m-%s\",\n  \"name\": \"%s\",\n  \"created\": \"2026-10-01T12:00:00.000Z\"\n}\n", id, name);
}

static void
put_text (TestMemoryFs *fs, const char *path, const char *text)
{
  test_memory_fs_put (fs, path, text, strlen (text));
}

static void
seed (Run *run, const char *id, const char *title, BroJobState state, const char *drive_id, const BroJsonValue *generation, gboolean automatic,
      const BroJsonValue *rows, gint64 created_ms)
{
  BroStore *store = BRO_STORE (run->r->store);
  g_autoptr (BroJobRecord) r = bro_job_record_new ();
  g_autoptr (BroBroError) error = NULL;
  gboolean finished = state == BRO_JOB_STATE_FINISHED;
  BroInstant created = { created_ms };
  g_strlcpy (r->id.value, id, sizeof r->id.value);
  r->kind = BRO_JOB_KIND_VIDEO_DISC;
  r->state = state;
  r->has_outcome = finished;
  r->outcome = BRO_OUTCOME_SUCCEEDED;
  r->queue = BRO_QUEUE_ACQUISITION;
  r->drive_id = g_strdup (drive_id);
  r->has_media_generation = generation && generation->kind == BRO_JSON_VALUE_INTEGER;
  r->media_generation = r->has_media_generation ? generation->integer : 0;
  r->automatic = automatic;
  r->title = g_strdup (title);
  r->request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, title);
  r->request->automatic = automatic;
  r->request->drive_id = g_strdup (drive_id);
  r->request->has_media_generation = r->has_media_generation;
  r->request->media_generation = r->media_generation;
  r->created_at = created;
  r->has_finished_at = finished;
  r->finished_at = created;
  g_assert_true (bro_job_repository_insert (bro_store_jobs (store), r, &error));
  for (guint i = 0; rows && i < bro_json_value_length (rows); i++)
    {
      BroJsonValue *row = bro_json_value_at (rows, i);
      g_autoptr (BroStepRecord) s = bro_step_record_new ();
      s->job_id = r->id;
      s->seq = (int) i;
      g_assert_true (bro_step_kind_from_wire (S (row, "kind", ""), &s->kind));
      g_assert_true (bro_step_state_from_wire (S (row, "state", ""), &s->state));
      s->attempt = 1;
      s->has_started_at = TRUE;
      s->started_at = created;
      s->has_finished_at = s->state != BRO_STEP_STATE_RUNNING;
      s->finished_at = created;
      if (s->state == BRO_STEP_STATE_SUCCEEDED)
        {
          g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
          s->output = bro_step_output_new (s->kind, empty, NULL);
        }
      g_assert_true (bro_step_repository_save (bro_store_steps (store), s, &error));
    }
}

static Run *
run_new (BroJsonValue *given)
{
  Run *run = g_new0 (Run, 1);
  BroJsonValue *config = J (given, "config"), *v;
  g_autoptr (BroJsonValue) default_config = NULL;
  g_autoptr (GBytes) config_bytes = NULL;
  g_autofree char *library_marker = marker_text ("lib", "Library");
  g_autoptr (BroJsonValue) disc = NULL;
  g_autofree char *listing = NULL;
  int n = 0;
  gint64 created_ms;
  g_mutex_init (&run->lock);
  run->jobs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  run->feeds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  run->seqs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  run->holds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  run->r = test_composition_root_new (bro_json_value_get_bool (J (given, "makemkvcon"), TRUE), S (given, "makemkvVersion", NULL),
                                      bro_json_value_get_bool (J (given, "mkvmerge"), TRUE), NULL, 8,
                                      (guint) bro_json_value_get_integer (J (given, "streamCapacity"), 1024), NULL);
  if (!config)
    {
      default_config = bro_json_value_parse (
          "{\"version\": 3, \"libraries\": [{\"id\": \"lib\", \"name\": \"Library\", \"path\": \"/root/library\"}]}", -1);
      config = default_config;
    }
  config_bytes = bro_json_value_encode_canonical (config);
  test_memory_fs_put (run->r->fs, TEST_ROOT_CONFIG_PATH, g_bytes_get_data (config_bytes, NULL), g_bytes_get_size (config_bytes));
  if (bro_json_value_get_bool (J (given, "configUnreadable"), FALSE))
    {
      g_autoptr (BroBroError) e = fs_failed ("read", TEST_ROOT_CONFIG_PATH);
      test_memory_fs_fail_reads (run->r->fs, TEST_ROOT_CONFIG_PATH, e);
    }
  put_text (run->r->fs, "/root/library" MARKER, library_marker);
  if ((v = J (given, "markers")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        const char *library = bro_json_value_get_string (v->items->pdata[i], "");
        g_autofree char *path = g_strconcat (v->keys->pdata[i], MARKER, NULL);
        g_autofree char *text = marker_text (library, library);
        put_text (run->r->fs, path, text);
      }
  for (guint i = 0; (v = J (given, "folders")) && i < bro_json_value_length (v); i++)
    test_memory_fs_put_folder (run->r->fs, bro_json_value_get_string (bro_json_value_at (v, i), ""));
  if (bro_json_value_get_bool (J (given, "slowMarkerRead"), FALSE))
    run->r->fs->on_read = slow_marker;

  {
    g_autoptr (BroOsDrive) bd = bro_os_drive_new ("/dev/disk4", "HL-DT-ST BD-RE WH16NS60", "/Volumes/MOVIE_DISC");
    g_autoptr (BroOsDrive) dvd = bro_os_drive_new ("/dev/disk6", "MATSHITA DVD-R UJ8C2", NULL);
    test_fake_drives_add (run->r->drives, bd);
    test_fake_drives_add (run->r->drives, dvd);
  }
  if ((v = J (given, "content")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        BroDiscContent content;
        g_assert_true (bro_disc_content_from_wire (bro_json_value_get_string (v->items->pdata[i], ""), &content));
        test_fake_drives_set_content (run->r->drives, v->keys->pdata[i], content);
      }

  if ((v = J (given, "scan")))
    {
      run->scan = g_new0 (char *, bro_json_value_length (v) + 1);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        run->scan[i] = g_strdup (bro_json_value_get_string (bro_json_value_at (v, i), ""));
    }
  else
    run->scan = g_strdupv ((char **) default_scan);
  disc = bro_json_value_parse ("{\"volume\": \"MOVIE_DISC\", \"type\": \"bluray\", \"titles\": [{\"duration\": \"1:40:00\", \"source\": 1, "
                               "\"chapters\": 12, \"size\": 30000000000}]}",
                               -1);
  listing = bro_test_generated_listing (disc);
  {
    g_auto (GStrv) all = g_strsplit (listing, "\n", -1);
    GStrvBuilder *b = g_strv_builder_new ();
    for (guint i = 0; all[i]; i++)
      if (*all[i])
        g_strv_builder_add (b, all[i]);
    run->listing = g_strv_builder_end (b);
    g_strv_builder_unref (b);
  }
  run->slow_scan = bro_json_value_get_bool (J (given, "slowDriveScan"), FALSE) ? 0 : 1;
  run->r->launcher->lines_for = lines_for;
  run->r->launcher->lines_for_data = run;
  for (guint i = 0; (v = J (given, "holds")) && i < bro_json_value_length (v); i++)
    g_hash_table_add (run->holds, g_strdup (bro_json_value_get_string (bro_json_value_at (v, i), "")));
  run->r->launcher->runs_until_stopped = runs_until_stopped;
  run->r->launcher->runs_until_stopped_data = run;
  run->r->launcher->stop_takes = bro_json_value_get_number (J (given, "stopTakes"), 0);
  run->r->on_release = released;
  run->r->on_release_data = run;

  created_ms = TEST_MANUAL_CLOCK_START_MS - 3600000;
  for (gint64 i = 0; i < bro_json_value_get_integer (J (given, "finishedJobs"), 0); i++)
    {
      g_autofree char *id = g_strdup_printf (SEEDED "%02x", ++n);
      g_autofree char *title = g_strdup_printf ("Done %" G_GINT64_FORMAT, i);
      g_autoptr (BroJsonValue) none = bro_json_value_new_array ();
      seed (run, id, title, BRO_JOB_STATE_FINISHED, NULL, NULL, FALSE, none, created_ms + i * 1000);
    }
  for (guint i = 0; (v = J (given, "jobs")) && i < bro_json_value_length (v); i++)
    {
      BroJsonValue *j = bro_json_value_at (v, i);
      g_autofree char *id = g_strdup_printf (SEEDED "%02x", ++n);
      BroJobState state;
      g_assert_true (bro_job_state_from_wire (S (j, "state", ""), &state));
      g_hash_table_replace (run->jobs, g_strdup (S (j, "name", "")), g_strdup (id));
      seed (run, id, S (j, "title", ""), state, S (j, "driveId", NULL), J (j, "mediaGeneration"),
            bro_json_value_get_bool (J (j, "automatic"), FALSE), J (j, "rows"), created_ms + 500000 + n * 1000);
    }
  return run;
}

/* Stops like the engine: the feeds closed, the engine stopped and settled, then everything dropped. */
static void
run_free (Run *run)
{
  GHashTableIter it;
  gpointer feed;
  g_hash_table_iter_init (&it, run->feeds);
  while (g_hash_table_iter_next (&it, NULL, &feed))
    bro_event_feed_close (feed);
  if (run->stopping)
    test_waiter_wait (run->stopping, STOP_PATIENCE);
  g_clear_pointer (&run->stopping, test_waiter_unref);
  test_composition_root_free (run->r);
  g_hash_table_unref (run->feeds);
  g_hash_table_unref (run->jobs);
  g_hash_table_unref (run->seqs);
  g_hash_table_unref (run->holds);
  g_strfreev (run->scan);
  g_strfreev (run->listing);
  g_free (run->failure);
  g_mutex_clear (&run->lock);
  g_free (run);
}

/* ---- matching ----------------------------------------------------------------------------------------------------- */

/* @text with {name} replaced by each named job's id and each named seq. */
static char *
subst_text (Run *run, const char *text)
{
  g_autoptr (GString) s = g_string_new (text);
  GHashTableIter it;
  gpointer k, v;
  for (int pass = 0; pass < 2; pass++)
    {
      g_hash_table_iter_init (&it, pass ? run->seqs : run->jobs);
      while (g_hash_table_iter_next (&it, &k, &v))
        {
          g_autofree char *pattern = g_strconcat ("{", (char *) k, "}", NULL);
          g_string_replace (s, pattern, v, 0);
        }
    }
  return g_strdup (s->str);
}

static BroJsonValue *
subst (Run *run, BroJsonValue *v)
{
  BroJsonValue *c;
  switch (v->kind)
    {
    case BRO_JSON_VALUE_STRING:
      {
        g_autofree char *t = subst_text (run, v->string);
        return bro_json_value_new_string (t);
      }
    case BRO_JSON_VALUE_ARRAY:
      c = bro_json_value_new_array ();
      for (guint i = 0; i < v->items->len; i++)
        bro_json_value_append (c, subst (run, v->items->pdata[i]));
      return c;
    case BRO_JSON_VALUE_OBJECT:
      c = bro_json_value_new_object ();
      for (guint i = 0; i < v->keys->len; i++)
        bro_json_value_set (c, v->keys->pdata[i], subst (run, v->items->pdata[i]));
      return c;
    default:
      return bro_json_value_ref (v);
    }
}

/* Objects: every wanted member matches (a wanted null: absent or null); exact also wants no others. Arrays: the same
 * length, element by element. Numbers by value. Anything else: equal. */
static gboolean
matches (const BroJsonValue *want, const BroJsonValue *got, gboolean exact)
{
  switch (want->kind)
    {
    case BRO_JSON_VALUE_NULL:
      return !got || got->kind == BRO_JSON_VALUE_NULL;
    case BRO_JSON_VALUE_OBJECT:
      if (!got || got->kind != BRO_JSON_VALUE_OBJECT || (exact && got->keys->len != want->keys->len))
        return FALSE;
      for (guint i = 0; i < want->keys->len; i++)
        if (!matches (want->items->pdata[i], J (got, want->keys->pdata[i]), exact))
          return FALSE;
      return TRUE;
    case BRO_JSON_VALUE_ARRAY:
      if (!got || got->kind != BRO_JSON_VALUE_ARRAY || got->items->len != want->items->len)
        return FALSE;
      for (guint i = 0; i < want->items->len; i++)
        if (!matches (want->items->pdata[i], got->items->pdata[i], exact))
          return FALSE;
      return TRUE;
    case BRO_JSON_VALUE_INTEGER:
    case BRO_JSON_VALUE_NUMBER:
      return got && (got->kind == BRO_JSON_VALUE_INTEGER || got->kind == BRO_JSON_VALUE_NUMBER)
             && bro_json_value_get_number (want, 0) == bro_json_value_get_number (got, 1);
    default:
      return got && bro_json_value_equal (want, got);
    }
}

static gboolean
same (Run *run, const BroJsonValue *want, const BroJsonValue *got, const char *what, gboolean exact)
{
  g_autofree char *w = NULL, *g = NULL;
  if (matches (want, got, exact))
    return TRUE;
  w = one_line (want);
  g = one_line (got);
  step_failed (run, "%s: wanted %s\n got %s", what, w, g);
  return FALSE;
}

static BroJsonValue *
error_json (const BroBroError *e)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "code", bro_json_value_new_string (e->code));
  if (bro_json_value_length (e->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (e->params));
  return o;
}

/* ---- calls -------------------------------------------------------------------------------------------------------- */

static void
status_done (const BroSystemStatusDto *status, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, status ? bro_system_status_dto_json (status) : NULL, NULL, error);
}

static void
job_done (const BroJobDto *job, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, job ? bro_job_dto_json (job) : NULL, NULL, error);
}

static void
page_done (const BroJobPageDto *page, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, page ? bro_job_page_dto_json (page) : NULL, NULL, error);
}

static void
detail_done (const BroJobDetailDto *detail, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, detail ? bro_job_detail_dto_json (detail) : NULL, NULL, error);
}

static void
library_done (const BroLibraryDto *library, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, library ? bro_library_dto_json (library) : NULL, NULL, error);
}

static void
envelope_done (const BroConfigEnvelope *envelope, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, envelope ? bro_config_envelope_json (envelope) : NULL, NULL, error);
}

static void
value_done (const BroJsonValue *value, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, value ? bro_json_value_ref ((BroJsonValue *) value) : NULL, NULL, error);
}

static void
feed_done (BroEventFeed *feed, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, NULL, feed ? g_object_ref (feed) : NULL, error);
}

static BroJsonValue *
list_json (GPtrArray *items, BroJsonValue *(*json) (gconstpointer))
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < items->len; i++)
    bro_json_value_append (a, json (g_ptr_array_index (items, i)));
  return a;
}

#define LIST_DONE(name, json)                                                                                          \
  static void name (GPtrArray *items, const BroBroError *error, gpointer data)                                       \
  {                                                                                                                    \
    test_waiter_finish (data, items ? list_json (items, (BroJsonValue * (*) (gconstpointer)) json) : NULL, NULL, error); \
  }
LIST_DONE (drives_done, bro_drive_dto_json)
LIST_DONE (log_done, bro_log_line_dto_json)
LIST_DONE (libraries_done, bro_library_dto_json)
LIST_DONE (issues_done, bro_issue_to_json)

/* The id of the job @name, else a job no one has. */
static BroId
job_of (Run *run, const BroJsonValue *name)
{
  const char *id = g_hash_table_lookup (run->jobs, bro_json_value_get_string (name, ""));
  BroId out = { { 0 } };
  g_strlcpy (out.value, id ? id : NOBODY, sizeof out.value);
  return out;
}

static BroRipOptionsDto *
options_of (const BroJsonValue *o)
{
  BroRipOptionsDto *options = bro_rip_options_dto_new ();
  BroJsonValue *titles = J (o, "titles");
  options->has_mode = bro_rip_mode_from_wire (S (o, "mode", ""), &options->mode);
  options->profile = g_strdup (S (o, "profile", NULL));
  options->library = g_strdup (S (o, "library", NULL));
  if (titles && titles->kind == BRO_JSON_VALUE_ARRAY)
    {
      options->titles = g_array_new (FALSE, FALSE, sizeof (int));
      for (guint i = 0; i < bro_json_value_length (titles); i++)
        {
          int t = (int) bro_json_value_get_integer (bro_json_value_at (titles, i), 0);
          g_array_append_val (options->titles, t);
        }
    }
  options->output_folder = g_strdup (S (o, "outputFolder", NULL));
  return options;
}

static BroJobRequest *
request_of (const BroJsonValue *r)
{
  BroJobKind kind;
  BroJobRequest *request;
  BroJsonValue *generation = J (r, "mediaGeneration");
  g_assert_true (bro_job_kind_from_wire (S (r, "kind", ""), &kind));
  request = bro_job_request_new (kind, S (r, "title", ""));
  request->automatic = bro_json_value_get_bool (J (r, "automatic"), FALSE);
  request->drive_id = g_strdup (S (r, "driveId", NULL));
  request->has_media_generation = generation && generation->kind == BRO_JSON_VALUE_INTEGER;
  request->media_generation = bro_json_value_get_integer (generation, 0);
  request->options = J (r, "options") ? bro_json_value_ref (J (r, "options")) : NULL;
  request->details = J (r, "details") ? bro_json_value_ref (J (r, "details")) : NULL;
  return request;
}

/* Starts the operation; NULL for one the harness doesn't know. */
static TestWaiter *
call_start (Run *run, const char *op, BroJsonValue *a)
{
  BroEngineAPI *e = BRO_ENGINE_API (run->r->engine);
  TestWaiter *w = test_waiter_new ();
  gpointer d = test_waiter_ref (w);
#define ARG(i) bro_json_value_at (a, (i))
#define ARG_S(i) bro_json_value_get_string (ARG (i), "")
  if (!strcmp (op, "system"))
    bro_engine_api_system (e, status_done, d, test_waiter_unref);
  else if (!strcmp (op, "stop"))
    bro_engine_api_stop (e, test_waiter_done, d, test_waiter_unref);
  else if (!strcmp (op, "drives"))
    bro_engine_api_drives (e, drives_done, d, test_waiter_unref);
  else if (!strcmp (op, "eject"))
    bro_engine_api_eject (e, ARG_S (0), test_waiter_done, d, test_waiter_unref);
  else if (!strcmp (op, "closeTray"))
    bro_engine_api_close_tray (e, ARG_S (0), test_waiter_done, d, test_waiter_unref);
  else if (!strcmp (op, "closeAllTrays"))
    bro_engine_api_close_all_trays (e, test_waiter_done, d, test_waiter_unref);
  else if (!strcmp (op, "rip"))
    {
      g_autoptr (BroRipOptionsDto) options = options_of (ARG (1));
      bro_engine_api_rip (e, ARG_S (0), options, job_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "jobs"))
    {
      g_autoptr (BroJobQueryDto) query = bro_job_query_dto_new ();
      bro_engine_api_jobs (e, query, page_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "job"))
    {
      BroId id = job_of (run, ARG (0));
      bro_engine_api_job (e, &id, detail_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "jobLog"))
    {
      BroId id = job_of (run, ARG (0));
      bro_engine_api_job_log (e, &id, bro_json_value_get_integer (ARG (1), 0), log_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "cancel"))
    {
      BroId id = job_of (run, ARG (0));
      bro_engine_api_cancel (e, &id, test_waiter_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "retry"))
    {
      BroId id = job_of (run, ARG (0));
      BroStepKind kind;
      gboolean has = ARG (1) && ARG (1)->kind == BRO_JSON_VALUE_STRING && bro_step_kind_from_wire (ARG_S (1), &kind);
      bro_engine_api_retry (e, &id, has ? &kind : NULL, job_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "keepApart"))
    {
      BroId id = job_of (run, ARG (0));
      bro_engine_api_keep_apart (e, &id, test_waiter_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "decide"))
    {
      BroId id = job_of (run, ARG (0));
      bro_engine_api_decide (e, &id, ARG_S (1), ARG_S (2), test_waiter_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "move"))
    {
      BroId id = job_of (run, ARG (0));
      bro_engine_api_move (e, &id, (int) bro_json_value_get_integer (ARG (1), 0), test_waiter_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "startNow"))
    {
      BroId id = job_of (run, ARG (0));
      bro_engine_api_start_now (e, &id, test_waiter_done, d, test_waiter_unref);
    }
  else if (!strcmp (op, "libraries"))
    bro_engine_api_libraries (e, libraries_done, d, test_waiter_unref);
  else if (!strcmp (op, "attachLibrary"))
    bro_engine_api_attach_library (e, ARG_S (0), ARG_S (1), library_done, d, test_waiter_unref);
  else if (!strcmp (op, "config"))
    bro_engine_api_config (e, envelope_done, d, test_waiter_unref);
  else if (!strcmp (op, "updateConfig"))
    bro_engine_api_update_config (e, ARG (0), bro_json_value_get_integer (ARG (1), 0), envelope_done, d, test_waiter_unref);
  else if (!strcmp (op, "validateConfig"))
    bro_engine_api_validate_config (e, ARG (0), issues_done, d, test_waiter_unref);
  else if (!strcmp (op, "setSecret"))
    bro_engine_api_set_secret (e, ARG_S (0), ARG_S (1), test_waiter_done, d, test_waiter_unref);
  else if (!strcmp (op, "exportConfig"))
    bro_engine_api_export_config (e, value_done, d, test_waiter_unref);
  else if (!strcmp (op, "startJob"))
    {
      g_autoptr (BroJobRequest) request = request_of (ARG (0));
      bro_local_engine_start_job (run->r->engine, request, job_done, d, test_waiter_unref);
    }
  else
    {
      test_waiter_unref (d);
      test_waiter_unref (w);
      return NULL;
    }
#undef ARG
#undef ARG_S
  return w;
}

/* Waits for the call; checks its result against expect, or its failure against error. The result (a reference,
 * nullable) through @result. */
static gboolean
expect_answer (Run *run, BroJsonValue *step, TestWaiter *w, BroJsonValue **result)
{
  BroJsonValue *want = J (step, "expect"), *want_error = J (step, "error");
  if (!test_waiter_wait (w, PATIENCE))
    {
      step_failed (run, "the call never ended");
      return FALSE;
    }
  if (w->error)
    {
      g_autoptr (BroJsonValue) got = error_json (w->error);
      g_autoptr (BroJsonValue) wanted = NULL;
      if (!want_error)
        {
          g_autofree char *text = one_line (got);
          step_failed (run, "failed: %s", text);
          return FALSE;
        }
      wanted = subst (run, want_error);
      return same (run, wanted, got, "error", FALSE);
    }
  if (want_error)
    {
      g_autofree char *e = one_line (want_error), *g = one_line (w->result);
      step_failed (run, "no error, wanted %s; got %s", e, g);
      return FALSE;
    }
  if (want && want->kind != BRO_JSON_VALUE_NULL)
    {
      g_autoptr (BroJsonValue) wanted = subst (run, want);
      if (!same (run, wanted, w->result, "result", FALSE))
        return FALSE;
    }
  if (result)
    *result = w->result ? bro_json_value_ref (w->result) : NULL;
  return TRUE;
}

static gboolean
step_call (Run *run, const char *op, BroJsonValue *step)
{
  g_autoptr (BroJsonValue) none = bro_json_value_new_array ();
  BroJsonValue *args = J (step, "args") ? J (step, "args") : none, *includes;
  g_autoptr (TestWaiter) w = call_start (run, op, args);
  g_autoptr (BroJsonValue) result = NULL;
  const char *as = S (step, "as", NULL), *id;
  if (!w)
    {
      step_failed (run, "unknown operation %s", op);
      return FALSE;
    }
  if (!expect_answer (run, step, w, &result))
    return FALSE;
  if (as && result && (id = S (result, "id", NULL)))
    g_hash_table_replace (run->jobs, g_strdup (as), g_strdup (id));
  if ((includes = J (step, "includes")))
    for (guint i = 0; i < bro_json_value_length (includes); i++)
      {
        g_autoptr (BroJsonValue) want = subst (run, bro_json_value_at (includes, i));
        gboolean found = FALSE;
        for (guint k = 0; result && k < bro_json_value_length (result) && !found; k++)
          found = matches (want, bro_json_value_at (result, k), FALSE);
        if (!found)
          {
            g_autofree char *w_text = one_line (want), *r_text = one_line (result);
            step_failed (run, "no line like %s in %s", w_text, r_text);
            return FALSE;
          }
      }
  return TRUE;
}

/* ---- the other steps ---------------------------------------------------------------------------------------------- */

static void
next_done (const BroEngineEvent *event, gpointer data)
{
  test_waiter_finish (data, event ? bro_engine_event_json (event) : NULL, NULL, NULL);
}

/* The feed's next event as JSON (NULL: the feed ended), FALSE when none came within @seconds. */
static gboolean
feed_next (BroEventFeed *feed, double seconds, BroJsonValue **event)
{
  g_autoptr (TestWaiter) w = test_waiter_new ();
  bro_event_feed_next (feed, next_done, test_waiter_ref (w), test_waiter_unref);
  if (!test_waiter_wait (w, seconds))
    return FALSE;
  *event = w->result ? bro_json_value_ref (w->result) : NULL;
  return TRUE;
}

/* Until the executor's activity is the held processes' (each one a running step's worker), seen three times in a
 * row. */
static gboolean
settle (Run *run)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) (PATIENCE * G_USEC_PER_SEC);
  int seen = 0;
  while (seen < 3)
    {
      int activity, held;
      if (g_get_monotonic_time () > deadline)
        {
          step_failed (run, "never settled: activity %d, held %d", bro_engine_executor_activity (run->r->executor),
                       test_scripted_launcher_held (run->r->launcher));
          return FALSE;
        }
      g_usleep (5000);
      activity = bro_engine_executor_activity (run->r->executor);
      held = test_scripted_launcher_held (run->r->launcher);
      seen = activity == held ? seen + 1 : 0;
    }
  return TRUE;
}

static gboolean
wait_stop (Run *run, TestWaiter *w, const char *what)
{
  if (!test_waiter_wait (w, STOP_PATIENCE))
    {
      step_failed (run, "%s never ended", what);
      return FALSE;
    }
  return TRUE;
}

static gboolean
step_next (Run *run, BroJsonValue *step, const char *name)
{
  BroEventFeed *feed = g_hash_table_lookup (run->feeds, name);
  BroJsonValue *wants = J (step, "expect");
  for (guint i = 0; feed && i < bro_json_value_length (wants); i++)
    {
      BroJsonValue *want = bro_json_value_at (wants, i), *jobs, *count, *first;
      g_autoptr (BroJsonValue) e = NULL;
      g_autoptr (BroJsonValue) members = NULL;
      g_autoptr (BroJsonValue) wanted = NULL;
      if (!feed_next (feed, PATIENCE, &e))
        {
          step_failed (run, "the next event never ended");
          return FALSE;
        }
      if (want->kind == BRO_JSON_VALUE_NULL)
        {
          if (e)
            {
              g_autofree char *text = one_line (e);
              step_failed (run, "an event after the feed closed: %s", text);
              return FALSE;
            }
          continue;
        }
      if (!e)
        {
          step_failed (run, "the feed ended");
          return FALSE;
        }
      members = bro_json_value_new_object ();
      for (guint k = 0; k < want->keys->len; k++)
        if (strcmp (want->keys->pdata[k], "seqIsLast") && strcmp (want->keys->pdata[k], "jobCount") && strcmp (want->keys->pdata[k], "firstJob"))
          bro_json_value_set (members, want->keys->pdata[k], bro_json_value_ref (want->items->pdata[k]));
      wanted = subst (run, members);
      if (!same (run, wanted, e, "event", FALSE))
        return FALSE;
      if (bro_json_value_get_bool (J (want, "seqIsLast"), FALSE)
          && bro_json_value_get_integer (J (e, "seq"), -1) != bro_event_bus_last_seq (run->r->bus))
        {
          step_failed (run, "event seq %" G_GINT64_FORMAT ", last %" G_GINT64_FORMAT, bro_json_value_get_integer (J (e, "seq"), -1),
                       bro_event_bus_last_seq (run->r->bus));
          return FALSE;
        }
      jobs = J (J (e, "data"), "jobs");
      if ((count = J (want, "jobCount")) && bro_json_value_length (jobs) != (guint) count->integer)
        {
          step_failed (run, "snapshot jobs %u, wanted %" G_GINT64_FORMAT, bro_json_value_length (jobs), count->integer);
          return FALSE;
        }
      if ((first = J (want, "firstJob"))
          && g_strcmp0 (S (bro_json_value_at (jobs, 0), "id", NULL), g_hash_table_lookup (run->jobs, first->string)) != 0)
        {
          step_failed (run, "first snapshot job %s", S (bro_json_value_at (jobs, 0), "id", "none"));
          return FALSE;
        }
    }
  if (!feed)
    {
      step_failed (run, "no feed %s", name);
      return FALSE;
    }
  return TRUE;
}

static gboolean
step_drain (Run *run, BroJsonValue *step, const char *name)
{
  BroEventFeed *feed = g_hash_table_lookup (run->feeds, name);
  g_autoptr (GHashTable) logs_of = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) want = g_hash_table_new (g_str_hash, g_str_equal);
  BroJsonValue *names = J (step, "jobLogsOf");
  gboolean ok = TRUE;
  GHashTableIter it;
  gpointer k;
  if (!feed)
    {
      step_failed (run, "no feed %s", name);
      return FALSE;
    }
  for (;;)
    {
      g_autoptr (BroJsonValue) e = NULL;
      if (!feed_next (feed, 0.3, &e) || !e)
        break;
      if (!strcmp (S (e, "type", ""), "jobLog"))
        g_hash_table_add (logs_of, g_strdup (S (J (e, "data"), "jobId", "")));
    }
  for (guint i = 0; i < bro_json_value_length (names); i++)
    g_hash_table_add (want, g_hash_table_lookup (run->jobs, bro_json_value_get_string (bro_json_value_at (names, i), "")));
  ok = g_hash_table_size (want) == g_hash_table_size (logs_of);
  g_hash_table_iter_init (&it, want);
  while (ok && g_hash_table_iter_next (&it, &k, NULL))
    ok = k && g_hash_table_contains (logs_of, k);
  if (!ok)
    {
      g_autoptr (GString) got = g_string_new ("");
      g_hash_table_iter_init (&it, logs_of);
      while (g_hash_table_iter_next (&it, &k, NULL))
        g_string_append_printf (got, "%s%s", got->len ? ", " : "", (char *) k);
      step_failed (run, "jobLog events of %s", got->str);
    }
  return ok;
}

static gboolean
step_stored (Run *run, BroJsonValue *value)
{
  for (guint i = 0; i < value->keys->len; i++)
    {
      const char *id = g_hash_table_lookup (run->jobs, value->keys->pdata[i]);
      BroId job = { { 0 } };
      g_autoptr (BroJobRecord) record = NULL;
      g_autoptr (BroJsonValue) got = NULL;
      g_autofree char *what = g_strconcat ("stored ", (char *) value->keys->pdata[i], NULL);
      BroJsonValue *request;
      const BroJobRequest *r;
      g_strlcpy (job.value, id ? id : NOBODY, sizeof job.value);
      record = bro_job_repository_load (bro_store_jobs (BRO_STORE (run->r->store)), job, NULL);
      if (!record)
        {
          step_failed (run, "no record of %s", (char *) value->keys->pdata[i]);
          return FALSE;
        }
      r = record->request;
      request = bro_json_value_new_object ();
      bro_json_value_set (request, "kind", bro_json_value_new_string (bro_job_kind_to_wire (r->kind)));
      bro_json_value_set (request, "title", bro_json_value_new_string (r->title));
      bro_json_value_set (request, "automatic", bro_json_value_new_bool (r->automatic));
      bro_json_value_set (request, "driveId", r->drive_id ? bro_json_value_new_string (r->drive_id) : bro_json_value_new_null ());
      bro_json_value_set (request, "mediaGeneration",
                          r->has_media_generation ? bro_json_value_new_integer (r->media_generation) : bro_json_value_new_null ());
      bro_json_value_set (request, "options", r->options ? bro_json_value_ref (r->options) : bro_json_value_new_null ());
      got = bro_json_value_new_object ();
      bro_json_value_set (got, "mediaGeneration",
                          record->has_media_generation ? bro_json_value_new_integer (record->media_generation) : bro_json_value_new_null ());
      bro_json_value_set (got, "request", request);
      if (!same (run, value->items->pdata[i], got, what, FALSE))
        return FALSE;
    }
  return TRUE;
}

static gboolean
step_files (Run *run, BroJsonValue *value)
{
  for (int absent = 0; absent < 2; absent++)
    {
      BroJsonValue *paths = J (value, absent ? "absent" : "exist");
      for (guint i = 0; i < bro_json_value_length (paths); i++)
        {
          g_autofree char *path = subst_text (run, bro_json_value_get_string (bro_json_value_at (paths, i), ""));
          if (bro_file_system_exists (BRO_FILE_SYSTEM (run->r->fs), path) == absent)
            {
              step_failed (run, "%s %s", absent ? "there" : "missing", path);
              return FALSE;
            }
        }
    }
  return TRUE;
}

static gboolean
step_engine_log (Run *run, BroJsonValue *value)
{
  g_autoptr (GPtrArray) lines = test_recording_engine_log_lines (run->r->log);
  g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
  for (guint i = 0; i < lines->len; i++)
    bro_json_value_append (got, bro_bro_message_to_json (g_ptr_array_index (lines, i)));
  return same (run, value, got, "engine log", TRUE);
}

static gboolean
step_processes (Run *run, BroJsonValue *value)
{
  g_autoptr (GPtrArray) specs = test_scripted_launcher_specs (run->r->launcher);
  g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
  for (guint i = 0; i < specs->len; i++)
    {
      const BroProcessSpec *spec = g_ptr_array_index (specs, i);
      const char *source = "";
      g_autofree char *line = NULL;
      if (has_argument (spec, "disc:9999"))
        continue;
      for (guint k = 0; spec->arguments && spec->arguments[k] && !*source; k++)
        if (g_str_has_prefix (spec->arguments[k], "disc:") || g_str_has_prefix (spec->arguments[k], "iso:")
            || g_str_has_prefix (spec->arguments[k], "file:"))
          source = spec->arguments[k];
      line = g_strconcat (verb (spec) ? verb (spec) : "", " ", source, NULL);
      bro_json_value_append (got, bro_json_value_new_string (line));
    }
  return same (run, value, got, "processes", TRUE);
}

static gboolean
step_tray_calls (Run *run, BroJsonValue *value)
{
  g_autoptr (GPtrArray) calls = test_fake_drives_calls (run->r->drives);
  g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
  for (guint i = 0; i < calls->len; i++)
    bro_json_value_append (got, bro_json_value_new_string (g_ptr_array_index (calls, i)));
  return same (run, value, got, "tray calls", TRUE);
}

static gboolean
step_released (Run *run, BroJsonValue *value)
{
  g_autoptr (BroJsonValue) got = bro_json_value_new_object ();
  gboolean was;
  g_mutex_lock (&run->lock);
  was = run->released;
  bro_json_value_set (got, "executorStopped", bro_json_value_new_bool (run->released_executor_stopped));
  bro_json_value_set (got, "processesRunning", bro_json_value_new_integer (run->released_running));
  g_mutex_unlock (&run->lock);
  if (!was)
    {
      step_failed (run, "release never ran");
      return FALSE;
    }
  return same (run, value, got, "released", FALSE);
}

static gboolean
step_feed (Run *run, BroJsonValue *step, BroJsonValue *value)
{
  BroJsonValue *after = J (value, "after"), *names = J (value, "jobLog");
  g_autoptr (GArray) ids = g_array_new (FALSE, FALSE, sizeof (BroId));
  g_autoptr (TestWaiter) w = test_waiter_new ();
  gboolean has_after = after && (after->kind == BRO_JSON_VALUE_STRING || after->kind == BRO_JSON_VALUE_INTEGER);
  gint64 from = 0;
  if (after && after->kind == BRO_JSON_VALUE_STRING)
    {
      g_autofree char *text = subst_text (run, after->string);
      from = g_ascii_strtoll (text, NULL, 10);
    }
  else if (after && after->kind == BRO_JSON_VALUE_INTEGER)
    from = after->integer;
  for (guint i = 0; i < bro_json_value_length (names); i++)
    {
      BroId id = job_of (run, bro_json_value_at (names, i));
      g_array_append_val (ids, id);
    }
  bro_engine_api_events (BRO_ENGINE_API (run->r->engine), has_after, from, (const BroId *) ids->data, ids->len, feed_done, test_waiter_ref (w),
                         test_waiter_unref);
  if (!test_waiter_wait (w, PATIENCE))
    {
      step_failed (run, "the call never ended");
      return FALSE;
    }
  if (w->error)
    {
      step_failed (run, "events failed: %s", w->error->code);
      return FALSE;
    }
  g_hash_table_replace (run->feeds, g_strdup (S (step, "as", "")), g_object_ref (w->object));
  return TRUE;
}

static gboolean
step_limits_of (Run *run, BroJsonValue *step, BroJsonValue *value)
{
  g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (J (value, "config"));
  gsize size;
  const char *text = g_bytes_get_data (bytes, &size);
  g_autoptr (BroConfig) config = bro_config_codec_decode (text, (gssize) size, NULL);
  g_autoptr (BroSchedulerLimits) limits = bro_scheduler_limits_of (config, (int) bro_json_value_get_integer (J (value, "cores"), 0));
  g_autoptr (BroJsonValue) got = bro_json_value_new_object ();
  bro_json_value_set (got, "acquisitions", bro_json_value_new_integer (limits->acquisitions));
  bro_json_value_set (got, "processing", bro_json_value_new_integer (limits->processing));
  bro_json_value_set (got, "cpu", bro_json_value_new_integer (limits->cpu));
  bro_json_value_set (got, "ioPerVolume", bro_json_value_new_integer (limits->io_per_volume));
  return same (run, J (step, "expect"), got, "limits", TRUE);
}

static gboolean
step_system_seq_is_last (Run *run)
{
  g_autoptr (TestWaiter) w = test_waiter_new ();
  bro_engine_api_system (BRO_ENGINE_API (run->r->engine), status_done, test_waiter_ref (w), test_waiter_unref);
  if (!test_waiter_wait (w, PATIENCE) || !w->result)
    {
      step_failed (run, "system never answered");
      return FALSE;
    }
  if (bro_json_value_get_integer (J (w->result, "eventSeq"), -1) != bro_event_bus_last_seq (run->r->bus))
    {
      step_failed (run, "system eventSeq %" G_GINT64_FORMAT ", last %" G_GINT64_FORMAT, bro_json_value_get_integer (J (w->result, "eventSeq"), -1),
                   bro_event_bus_last_seq (run->r->bus));
      return FALSE;
    }
  return TRUE;
}

static gboolean
step (Run *run, BroJsonValue *s)
{
  const char *op = S (s, "call", NULL), *key;
  BroJsonValue *value;
  if (op)
    return step_call (run, op, s);
  key = s->keys->pdata[0];
  value = s->items->pdata[0];
  if (!strcmp (key, "start"))
    {
      g_autoptr (TestWaiter) w = test_composition_root_start (run->r);
      return expect_answer (run, s, w, NULL);
    }
  if (!strcmp (key, "stop"))
    {
      g_autoptr (TestWaiter) w = test_composition_root_stop (run->r);
      return wait_stop (run, w, "stop");
    }
  if (!strcmp (key, "stopBegins"))
    {
      run->stopping = test_composition_root_stop (run->r);
      g_usleep (50 * 1000);
      return TRUE;
    }
  if (!strcmp (key, "stopEnds"))
    return run->stopping ? wait_stop (run, run->stopping, "stop") : (step_failed (run, "no stop began"), FALSE);
  if (!strcmp (key, "monitoring"))
    {
      gboolean on = test_fake_drives_monitoring (run->r->drives);
      if (on != bro_json_value_get_bool (value, FALSE))
        step_failed (run, "monitoring %s", on ? "true" : "false");
      return !run->failure;
    }
  if (!strcmp (key, "timers"))
    {
      int pending = test_manual_clock_pending (run->r->clock);
      if (pending != bro_json_value_get_integer (value, -1))
        step_failed (run, "timers pending %d", pending);
      return !run->failure;
    }
  if (!strcmp (key, "restart"))
    {
      g_autoptr (BroBroError) error = NULL;
      if (!test_composition_root_restart (run->r, STOP_PATIENCE, &error, NULL))
        step_failed (run, "restart: %s", error ? error->code : "never ended");
      return !run->failure;
    }
  if (!strcmp (key, "settle"))
    return settle (run);
  if (!strcmp (key, "engineLog"))
    return step_engine_log (run, value);
  if (!strcmp (key, "processes"))
    return step_processes (run, value);
  if (!strcmp (key, "trayCalls"))
    return step_tray_calls (run, value);
  if (!strcmp (key, "files"))
    return step_files (run, value);
  if (!strcmp (key, "stored"))
    return step_stored (run, value);
  if (!strcmp (key, "released"))
    return step_released (run, value);
  if (!strcmp (key, "feed"))
    return step_feed (run, s, value);
  if (!strcmp (key, "next"))
    return step_next (run, s, bro_json_value_get_string (value, ""));
  if (!strcmp (key, "drain"))
    return step_drain (run, s, bro_json_value_get_string (value, ""));
  if (!strcmp (key, "seq"))
    {
      g_hash_table_replace (run->seqs, g_strdup (bro_json_value_get_string (value, "")),
                            g_strdup_printf ("%" G_GINT64_FORMAT, bro_event_bus_last_seq (run->r->bus)));
      return TRUE;
    }
  if (!strcmp (key, "fail"))
    {
      g_autoptr (BroBroError) e = fs_failed (S (value, "op", ""), S (value, "path", ""));
      test_memory_fs_fail (run->r->fs, S (value, "op", ""), S (value, "path", ""), e);
      return TRUE;
    }
  if (!strcmp (key, "unfail"))
    {
      test_memory_fs_unfail (run->r->fs);
      return TRUE;
    }
  if (!strcmp (key, "unmount") || !strcmp (key, "mount"))
    {
      gboolean ok = !strcmp (key, "mount") ? test_composition_root_mount (run->r, bro_json_value_get_string (value, ""), PATIENCE)
                                           : test_composition_root_unmount (run->r, bro_json_value_get_string (value, ""), PATIENCE);
      if (!ok)
        step_failed (run, "%s never ended", key);
      return ok;
    }
  if (!strcmp (key, "limitsOf"))
    return step_limits_of (run, s, value);
  if (!strcmp (key, "systemSeqIsLast"))
    return step_system_seq_is_last (run);
  step_failed (run, "unknown step %s", key);
  return FALSE;
}

static gboolean
local_engine_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  Run *run = run_new (given);
  BroJsonValue *steps = J (expect, "steps");
  for (guint i = 0; i < bro_json_value_length (steps); i++)
    {
      BroJsonValue *s = bro_json_value_at (steps, i);
      if (!step (run, s))
        {
          g_autofree char *text = one_line (s);
          bro_test_fail (failures, id, "step %u %s: %s", i, text, run->failure ? run->failure : "failed");
          break;
        }
    }
  run_free (run);
  return TRUE;
}

static void
test_local_engine (void)
{
  bro_test_run_cases ("engine/local-engine.cases.json", local_engine_case);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/local-engine", test_local_engine);
  return g_test_run ();
}
