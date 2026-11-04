/* bro-scenario-steps.c */
#include "bro-scenario-steps.h"
#include "bro-scenario-catalog.h"
#include "bro-scenario-expect.h"

#include "bro-disc-request.h"
#include "bro-drive-dto.h"
#include "bro-job-page-dto.h"
#include "bro-job-query-dto.h"
#include "bro-library-dto.h"
#include "bro-log-line-dto.h"
#include "bro-message-code.h"
#include "bro-rip-options-dto.h"
#include "bro-session-choices.h"
#include "bro-session-snapshot.h"
#include "bro-title-choice.h"

#include <string.h>

#define J(v, k) bro_json_value_member ((v), (k))
#define S(v, k, d) bro_json_value_get_string (J ((v), (k)), (d))

static gboolean run_point (const char *point, gpointer data, BroBroError **error);

/* ---- failures and text ---- */

gboolean
bro_scenario_run_fail (BroScenarioRun *run, const char *expected, const char *found)
{
  if (!run->failure)
    run->failure = bro_scenario_failure (expected, found);
  return FALSE;
}

gboolean
bro_scenario_run_failf (BroScenarioRun *run, const char *expected, const char *format, ...)
{
  va_list args;
  g_autofree char *found = NULL;
  va_start (args, format);
  found = g_strdup_vprintf (format, args);
  va_end (args);
  return bro_scenario_run_fail (run, expected, found);
}

char *
bro_scenario_json_text (const BroJsonValue *v)
{
  g_autofree char *text = NULL;
  GString *out;
  if (!v)
    return g_strdup ("null");
  text = bro_json_value_to_text (v);
  out = g_string_new (NULL);
  for (const char *c = text; *c;)
    {
      if (g_ascii_isspace (*c))
        {
          const char *end = c;
          gboolean line = FALSE;
          while (*end && g_ascii_isspace (*end))
            line |= *end++ == '\n';
          if (line)
            g_string_append_c (out, ' ');
          else
            g_string_append_len (out, c, end - c);
          c = end;
        }
      else
        g_string_append_c (out, *c++);
    }
  g_strstrip (out->str);
  out->len = strlen (out->str);
  return g_string_free (out, FALSE);
}

BroJsonValue *
bro_scenario_error_json (const BroBroError *e)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "code", bro_json_value_new_string (e->code));
  if (bro_json_value_length (e->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (e->params));
  return o;
}

char *
bro_scenario_path (const char *relative)
{
  g_autoptr (GString) p = g_string_new (relative);
  g_string_replace (p, "{root}", TEST_ROOT, 0);
  return p->str[0] == '/' ? g_strdup (p->str) : g_strconcat (TEST_ROOT "/", p->str, NULL);
}

static char *
fill_root (const char *s)
{
  g_autoptr (GString) p = g_string_new (s);
  g_string_replace (p, "{root}", TEST_ROOT, 0);
  return g_strdup (p->str);
}

/* ---- the engine's answers ---- */

static void
json_done (BroJsonValue *json, const BroBroError *error, gpointer data)
{
  test_waiter_finish (data, json, NULL, error);
}

static void
status_done (const BroSystemStatusDto *status, const BroBroError *error, gpointer data)
{
  json_done (status ? bro_system_status_dto_json (status) : NULL, error, data);
}

static void
job_done (const BroJobDto *job, const BroBroError *error, gpointer data)
{
  json_done (job ? bro_job_dto_json (job) : NULL, error, data);
}

static void
page_done (const BroJobPageDto *page, const BroBroError *error, gpointer data)
{
  json_done (page ? bro_job_page_dto_json (page) : NULL, error, data);
}

static void
detail_done (const BroJobDetailDto *detail, const BroBroError *error, gpointer data)
{
  json_done (detail ? bro_job_detail_dto_json (detail) : NULL, error, data);
}

static void
library_done (const BroLibraryDto *library, const BroBroError *error, gpointer data)
{
  json_done (library ? bro_library_dto_json (library) : NULL, error, data);
}

static void
envelope_done (const BroConfigEnvelope *envelope, const BroBroError *error, gpointer data)
{
  json_done (envelope ? bro_config_envelope_json (envelope) : NULL, error, data);
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
    json_done (items ? list_json (items, (BroJsonValue * (*) (gconstpointer)) json) : NULL, error, data);              \
  }
LIST_DONE (drives_done, bro_drive_dto_json)
LIST_DONE (log_done, bro_log_line_dto_json)
LIST_DONE (libraries_done, bro_library_dto_json)

/* The call's answer: its result (a reference; an empty object for none), or NULL with the failure "<kind> to succeed /
 * error {…}" (or one that never ended). */
static BroJsonValue *
answer (BroScenarioRun *run, TestWaiter *w)
{
  g_autofree char *expected = g_strconcat (run->step_kind, " to succeed", NULL);
  if (!test_waiter_wait (w, BRO_SCENARIO_PATIENCE))
    {
      bro_scenario_run_fail (run, expected, "the call never ended within 10 s");
      return NULL;
    }
  if (w->error)
    {
      g_autoptr (BroJsonValue) e = bro_scenario_error_json (w->error);
      g_autofree char *text = bro_scenario_json_text (e);
      bro_scenario_run_failf (run, expected, "error %s", text);
      return NULL;
    }
  return w->result ? bro_json_value_ref (w->result) : bro_json_value_new_object ();
}

#define CALL(run, start)                                                                                               \
  ({                                                                                                                   \
    g_autoptr (TestWaiter) _w = test_waiter_new ();                                                                    \
    gpointer d = test_waiter_ref (_w);                                                                                 \
    BroEngineAPI *e = BRO_ENGINE_API ((run)->root->engine);                                                            \
    (void) e;                                                                                                          \
    start;                                                                                                             \
    answer ((run), _w);                                                                                                \
  })

BroJsonValue *
bro_scenario_engine_job (BroScenarioRun *run, const BroId *id)
{
  return CALL (run, bro_engine_api_job (e, id, detail_done, d, test_waiter_unref));
}

BroJsonValue *
bro_scenario_engine_job_log (BroScenarioRun *run, const BroId *id)
{
  return CALL (run, bro_engine_api_job_log (e, id, 0, log_done, d, test_waiter_unref));
}

BroJsonValue *
bro_scenario_engine_drives (BroScenarioRun *run)
{
  return CALL (run, bro_engine_api_drives (e, drives_done, d, test_waiter_unref));
}

BroJsonValue *
bro_scenario_engine_system (BroScenarioRun *run)
{
  return CALL (run, bro_engine_api_system (e, status_done, d, test_waiter_unref));
}

BroJsonValue *
bro_scenario_engine_config (BroScenarioRun *run)
{
  return CALL (run, bro_engine_api_config (e, envelope_done, d, test_waiter_unref));
}

static gboolean
done_ok (BroScenarioRun *run, BroJsonValue *answered)
{
  if (!answered)
    return FALSE;
  bro_json_value_unref (answered);
  return TRUE;
}

/* ---- jobs and drives ---- */

gboolean
bro_scenario_run_job (BroScenarioRun *run, const char *name, BroId *id)
{
  const char *found = name ? g_hash_table_lookup (run->jobs, name) : NULL;
  if (!found)
    {
      g_autofree char *what = g_strconcat ("a job named ", name ? name : "?", NULL);
      return bro_scenario_run_fail (run, what, "none");
    }
  memset (id, 0, sizeof *id);
  g_strlcpy (id->value, found, sizeof id->value);
  return TRUE;
}

const char *
bro_scenario_run_only_job (BroScenarioRun *run)
{
  if (run->job_names->len == 1)
    return run->job_names->pdata[0];
  bro_scenario_run_failf (run, "one job, or a name", "%u jobs", run->job_names->len);
  return NULL;
}

static void
name_job (BroScenarioRun *run, const char *name, const char *id)
{
  if (!g_hash_table_contains (run->jobs, name))
    g_ptr_array_add (run->job_names, g_strdup (name));
  g_hash_table_replace (run->jobs, g_strdup (name), g_strdup (id));
}

const char *
bro_scenario_run_name_of (BroScenarioRun *run, const char *id)
{
  for (guint i = 0; i < run->job_names->len; i++)
    if (g_strcmp0 (g_hash_table_lookup (run->jobs, run->job_names->pdata[i]), id) == 0)
      return run->job_names->pdata[i];
  return id;
}

char *
bro_scenario_run_drive_id (BroScenarioRun *run, const char *name)
{
  BroScenarioDrive *drive = bro_scenario_world_drive_named (run->world, name, &run->failure);
  g_autoptr (BroJsonValue) drives = NULL;
  g_autofree char *what = NULL;
  if (!drive || !(drives = bro_scenario_engine_drives (run)))
    return NULL;
  for (guint i = 0; i < bro_json_value_length (drives); i++)
    if (g_strcmp0 (S (bro_json_value_at (drives, i), "device", NULL), drive->device) == 0)
      return g_strdup (S (bro_json_value_at (drives, i), "id", ""));
  what = g_strdup_printf ("drive %s (%s) in drives()", name, drive->device);
  bro_scenario_run_fail (run, what, "not listed");
  return NULL;
}

static int
by_created (gconstpointer a, gconstpointer b)
{
  const BroJsonValue *x = *(BroJsonValue *const *) a, *y = *(BroJsonValue *const *) b;
  int c = g_strcmp0 (S (x, "createdAt", ""), S (y, "createdAt", ""));
  return c ? c : g_strcmp0 (S (x, "id", ""), S (y, "id", ""));
}

BroJsonValue *
bro_scenario_run_all_jobs (BroScenarioRun *run)
{
  g_autoptr (GPtrArray) all = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_json_value_unref);
  g_autofree char *cursor = NULL;
  BroJsonValue *out;
  do
    {
      g_autoptr (BroJobQueryDto) query = bro_job_query_dto_new ();
      g_autoptr (BroJsonValue) page = NULL;
      BroJsonValue *items;
      query->cursor = g_strdup (cursor);
      query->limit = BRO_JOB_QUERY_DTO_MAX_LIMIT;
      if (!(page = CALL (run, bro_engine_api_jobs (e, query, page_done, d, test_waiter_unref))))
        return NULL;
      items = J (page, "items");
      for (guint i = 0; i < bro_json_value_length (items); i++)
        g_ptr_array_add (all, bro_json_value_ref (bro_json_value_at (items, i)));
      g_free (cursor);
      cursor = g_strdup (S (page, "next", NULL));
    }
  while (cursor);
  g_ptr_array_sort (all, by_created);
  out = bro_json_value_new_array ();
  for (guint i = 0; i < all->len; i++)
    bro_json_value_append (out, bro_json_value_ref (all->pdata[i]));
  return out;
}

/* ---- events ---- */

typedef struct {
  BroScenarioRun *run;
  BroEventFeed *feed; /* a reference */
  GThread *thread;
  gint stop;
} Drain;

static void
next_done (const BroEngineEvent *event, gpointer data)
{
  test_waiter_finish (data, event ? bro_engine_event_json (event) : NULL, NULL, NULL);
}

static gpointer
drain_thread (gpointer data)
{
  Drain *d = data;
  BroScenarioRun *run = d->run;
  for (;;)
    {
      g_autoptr (TestWaiter) w = test_waiter_new ();
      BroJsonValue *e;
      bro_event_feed_next (d->feed, next_done, test_waiter_ref (w), test_waiter_unref);
      while (!test_waiter_wait (w, 0.05))
        if (g_atomic_int_get (&d->stop))
          return NULL;
      if (!(e = w->result))
        return NULL;
      if (!strcmp (S (e, "type", ""), "snapshot"))
        continue;
      g_mutex_lock (&run->lock);
      if (run->drain == d && !(run->has_cutoff && bro_json_value_get_integer (J (e, "seq"), 0) > run->cutoff))
        g_ptr_array_add (run->events, bro_json_value_ref (e));
      g_mutex_unlock (&run->lock);
    }
}

/* Ends the current feed's drainer (its feed closed). */
static void
close_feed (BroScenarioRun *run)
{
  Drain *d;
  g_mutex_lock (&run->lock);
  d = run->drain;
  run->drain = NULL;
  g_mutex_unlock (&run->lock);
  if (!d)
    return;
  g_atomic_int_set (&d->stop, TRUE);
  bro_event_feed_close (d->feed);
  g_thread_join (d->thread);
  g_object_unref (d->feed);
  g_free (d);
}

/* Forgets the current feed without closing it (a stopped engine's: its feed ends by itself). */
static void
drop_feed (BroScenarioRun *run)
{
  close_feed (run);
}

static gboolean
open_feed (BroScenarioRun *run, gboolean has_after, gint64 after)
{
  g_autoptr (TestWaiter) w = test_waiter_new ();
  Drain *d;
  bro_engine_api_events (BRO_ENGINE_API (run->root->engine), has_after, after, NULL, 0, feed_done, test_waiter_ref (w), test_waiter_unref);
  if (!test_waiter_wait (w, BRO_SCENARIO_PATIENCE) || w->error || !w->object)
    return bro_scenario_run_fail (run, "the event feed to open", w->error ? w->error->code : "it never did");
  d = g_new0 (Drain, 1);
  d->run = run;
  d->feed = g_object_ref (w->object);
  g_mutex_lock (&run->lock);
  run->drain = d;
  run->has_cutoff = FALSE;
  g_mutex_unlock (&run->lock);
  d->thread = g_thread_new ("scenario-feed", drain_thread, d);
  return TRUE;
}

BroJsonValue *
bro_scenario_run_events (BroScenarioRun *run)
{
  BroJsonValue *copy = bro_json_value_new_array ();
  g_mutex_lock (&run->lock);
  for (guint i = 0; i < run->events->len; i++)
    bro_json_value_append (copy, bro_json_value_ref (run->events->pdata[i]));
  g_mutex_unlock (&run->lock);
  return copy;
}

/* Until the feed has delivered what the bus published (it is read on its own thread and leaves out the jobLog events,
 * so it can't be matched by seq): no new event three times in a row, 5 ms apart. */
static void
catch_up (BroScenarioRun *run)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) (BRO_SCENARIO_PATIENCE * G_USEC_PER_SEC);
  int seen = 0;
  guint count = G_MAXUINT;
  while (seen < 3 && g_get_monotonic_time () < deadline)
    {
      guint now;
      g_usleep (5000);
      g_mutex_lock (&run->lock);
      now = run->events->len;
      g_mutex_unlock (&run->lock);
      seen = now == count ? seen + 1 : 0;
      count = now;
    }
}

/* ---- settling ---- */

/* The jobs whose latest jobChanged or jobFinished says awaitingDecision. */
static int
awaiting_decisions (BroScenarioRun *run)
{
  g_autoptr (GHashTable) states = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTableIter it;
  gpointer v;
  int n = 0;
  g_mutex_lock (&run->lock);
  for (guint i = 0; i < run->events->len; i++)
    {
      BroJsonValue *e = run->events->pdata[i], *job = J (J (e, "data"), "job");
      const char *type = S (e, "type", "");
      if ((!strcmp (type, "jobChanged") || !strcmp (type, "jobFinished")) && S (job, "id", NULL))
        g_hash_table_replace (states, (gpointer) S (job, "id", NULL), (gpointer) S (job, "state", ""));
    }
  g_hash_table_iter_init (&it, states);
  while (g_hash_table_iter_next (&it, NULL, &v))
    n += !strcmp (v, "awaitingDecision");
  g_mutex_unlock (&run->lock);
  return n;
}

/* The executor's activity is only the parked workers, three times in a row: the parked processes' and, for each job
 * awaiting a decision, its step waiting for the answer. */
static gboolean
quiet (BroScenarioRun *run)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) (BRO_SCENARIO_PATIENCE * G_USEC_PER_SEC);
  int seen = 0;
  while (seen < 3)
    {
      if (g_get_monotonic_time () > deadline)
        return bro_scenario_run_failf (run, "the engine to go quiet", "activity %d, parked processes %d, questions %d",
                                       bro_engine_executor_activity (run->root->executor), bro_scenario_tools_parked (run->tools),
                                       awaiting_decisions (run));
      g_usleep (5000);
      seen = bro_engine_executor_activity (run->root->executor) == bro_scenario_tools_parked (run->tools) + awaiting_decisions (run) ? seen + 1 : 0;
    }
  return TRUE;
}

static gboolean
is_settled (const char *state)
{
  return !strcmp (state, "finished") || !strcmp (state, "blocked") || !strcmp (state, "awaitingDecision") || !strcmp (state, "waitingForResources");
}

static gboolean
killed_yet (BroScenarioRun *run)
{
  gboolean k;
  g_mutex_lock (&run->lock);
  k = run->killed != NULL;
  g_mutex_unlock (&run->lock);
  return k;
}

gboolean
bro_scenario_run_settle (BroScenarioRun *run)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) (BRO_SCENARIO_PATIENCE * G_USEC_PER_SEC);
  for (;;)
    {
      g_autoptr (BroJsonValue) jobs = NULL;
      g_autoptr (GString) stuck = NULL;
      BroInstant due;
      gint64 now;
      if (killed_yet (run))
        return TRUE;
      if (!quiet (run))
        return FALSE;
      if (killed_yet (run))
        return TRUE;
      if (!(jobs = bro_scenario_run_all_jobs (run)))
        return FALSE;
      for (guint i = 0; i < bro_json_value_length (jobs); i++)
        {
          BroJsonValue *j = bro_json_value_at (jobs, i);
          if (is_settled (S (j, "state", "")))
            continue;
          if (!stuck)
            stuck = g_string_new ("stuck: ");
          else
            g_string_append (stuck, ", ");
          g_string_append_printf (stuck, "%s %s", bro_scenario_run_name_of (run, S (j, "id", "")), S (j, "state", ""));
          if (J (J (j, "progress"), "step"))
            g_string_append_printf (stuck, " at %s", S (J (j, "progress"), "step", ""));
        }
      if (!stuck)
        {
          g_autoptr (BroJsonValue) again = NULL;
          gboolean all = TRUE;
          if (!quiet (run) || !(again = bro_scenario_run_all_jobs (run)))
            return FALSE;
          for (guint i = 0; i < bro_json_value_length (again); i++)
            all &= is_settled (S (bro_json_value_at (again, i), "state", ""));
          if (all)
            {
              catch_up (run);
              return TRUE;
            }
          continue;
        }
      if (g_get_monotonic_time () > deadline || !test_manual_clock_next_due (run->root->clock, &due))
        return bro_scenario_run_fail (run, "every job to settle", stuck->str);
      now = bro_clock_now (BRO_CLOCK (run->root->clock)).unix_milliseconds;
      test_manual_clock_advance (run->root->clock, MAX (0, due.unix_milliseconds - now), NULL, NULL);
    }
}

/* The clock moves to each due timer in turn (the engine going quiet after each), then to now + ms. */
static gboolean
advance (BroScenarioRun *run, gint64 ms)
{
  BroClock *clock = BRO_CLOCK (run->root->clock);
  gint64 target = bro_clock_now (clock).unix_milliseconds + ms;
  BroInstant due;
  if (!quiet (run))
    return FALSE;
  while (test_manual_clock_next_due (run->root->clock, &due) && due.unix_milliseconds <= target)
    {
      test_manual_clock_advance (run->root->clock, MAX (0, due.unix_milliseconds - bro_clock_now (clock).unix_milliseconds), NULL, NULL);
      if (!quiet (run))
        return FALSE;
    }
  test_manual_clock_advance (run->root->clock, MAX (0, target - bro_clock_now (clock).unix_milliseconds), NULL, NULL);
  if (!quiet (run))
    return FALSE;
  catch_up (run);
  return TRUE;
}

/* ---- crash points, kill, stop, restart ---- */

/* Arms the kill or stop point of the steps from @from up to the next restart, so the engine stops there however soon it
 * gets there (a job started by the step before a kill may reach its point before the kill step is played). */
static void
arm (BroScenarioRun *run, guint from)
{
  BroJsonValue *steps = J (run->scenario, "steps");
  g_mutex_lock (&run->lock);
  for (guint i = from; i < bro_json_value_length (steps); i++)
    {
      BroJsonValue *s = bro_json_value_at (steps, i);
      const char *kind = s->keys->len ? s->keys->pdata[0] : "";
      if (!strcmp (kind, "restart"))
        break;
      if (!strcmp (kind, "kill"))
        {
          g_free (run->kill_at);
          run->kill_at = g_strdup (S (s->items->pdata[0], "at", ""));
        }
      if (!strcmp (kind, "stop"))
        {
          g_free (run->hold_at);
          run->hold_at = g_strdup (S (s->items->pdata[0], "at", ""));
        }
    }
  g_mutex_unlock (&run->lock);
}

/* inStep:<kind> is inStep:<kind>:1. */
static char *
normal (const char *point)
{
  const char *c = point;
  int colons = 0;
  for (; *c; c++)
    colons += *c == ':';
  return g_str_has_prefix (point, "inStep:") && colons == 1 ? g_strconcat (point, ":1", NULL) : g_strdup (point);
}

static gboolean
same_point (const char *a, const char *b)
{
  g_autofree char *x = normal (a), *y = normal (b);
  return strcmp (x, y) == 0;
}

/* A step begins (its kind) or has ended (NULL): the tools' processes and the file operations from now on are its. */
static void
running (BroScenarioRun *run, const char *kind)
{
  bro_scenario_tools_step_begins (run->tools, kind);
  g_mutex_lock (&run->step_lock);
  g_free (run->running);
  run->running = g_strdup (kind);
  run->seal_wrote = FALSE;
  run->renames = 0;
  g_mutex_unlock (&run->step_lock);
}

/* One of given.faults: @op on a path @path (a glob relative to the root) fails with @reason, @left more times. */
typedef struct {
  char *op;
  char *path;
  char *reason;
  int left;
} ScenarioFault;

static void
fault_free (ScenarioFault *f)
{
  g_free (f->op);
  g_free (f->path);
  g_free (f->reason);
  g_free (f);
}

static gboolean
in_staging (const char *path)
{
  return strstr (path, "/.bromelia/staging/") != NULL;
}

/* The kit's hook before each file operation of the engine (decision 298; a tool's or the setup's writes don't come here).
 * A given fault fails it before it acts (owner I1-2), as the kit's failure rules fail; Seal's first write into staging is
 * seal.afterHash: every file is synced and hashed by then, and nothing of SHA256SUMS, the record or the logs is written.
 * Any thread. */
static gboolean
before_op (const char *op, const char *path, gpointer data, BroBroError **error)
{
  BroScenarioRun *run = data;
  const char *relative = g_str_has_prefix (path, TEST_ROOT "/") ? path + strlen (TEST_ROOT "/") : path;
  ScenarioFault *fault = NULL;
  gboolean first;
  g_mutex_lock (&run->step_lock);
  for (guint i = 0; i < run->faults->len && !fault; i++)
    {
      ScenarioFault *f = run->faults->pdata[i];
      if (f->left > 0 && strcmp (f->op, op) == 0 && bro_scenario_expect_glob (f->path, relative))
        fault = f;
    }
  if (fault)
    fault->left--;
  g_mutex_unlock (&run->step_lock);
  if (fault)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "operation", bro_json_value_new_string (op));
      bro_json_value_set (params, "path", bro_json_value_new_string (path));
      bro_json_value_set (params, "reason", bro_json_value_new_string (fault->reason));
      bro_bro_error_set (error, "fs.failed", params);
      return FALSE;
    }
  if (strcmp (op, "write") != 0 || !in_staging (path))
    return TRUE;
  g_mutex_lock (&run->step_lock);
  first = g_strcmp0 (run->running, "seal") == 0 && !run->seal_wrote;
  if (first)
    run->seal_wrote = TRUE;
  g_mutex_unlock (&run->step_lock);
  return !first || run_point ("seal.afterHash", run, error);
}

/* The kit's hook once a rename has landed: Name's n-th rename in staging is name.afterRename:<n> (owner I1-1; a rename
 * through a free name on the way is two). Any thread. */
static gboolean
after_rename (const char *from, gpointer data, BroBroError **error)
{
  BroScenarioRun *run = data;
  g_autofree char *point = NULL;
  if (!in_staging (from))
    return TRUE;
  g_mutex_lock (&run->step_lock);
  if (g_strcmp0 (run->running, "name") == 0)
    point = g_strdup_printf ("name.afterRename:%d", ++run->renames);
  g_mutex_unlock (&run->step_lock);
  return !point || run_point (point, run, error);
}

/* given.faults into run->faults, and the kit's hooks set; FALSE (a failure) for an op that isn't one. */
static gboolean
arm_faults (BroScenarioRun *run)
{
  BroJsonValue *faults = J (J (run->scenario, "given"), "faults");
  for (guint i = 0; i < bro_json_value_length (faults); i++)
    {
      BroJsonValue *f = bro_json_value_at (faults, i);
      const char *op = S (f, "op", "");
      ScenarioFault *fault;
      if (strcmp (op, "rename") != 0 && strcmp (op, "write") != 0 && strcmp (op, "sync") != 0 && strcmp (op, "remove") != 0)
        return bro_scenario_run_fail (run, "given.faults[].op rename, write, sync or remove", op);
      fault = g_new0 (ScenarioFault, 1);
      fault->op = g_strdup (op);
      fault->path = g_strdup (S (f, "path", ""));
      fault->reason = g_strdup (S (f, "error", ""));
      fault->left = (int) bro_json_value_get_integer (J (f, "times"), 1);
      g_ptr_array_add (run->faults, fault);
    }
  run->root->fs->on_op_data = run;
  run->root->fs->on_op = before_op;
  run->root->fs->on_renamed = after_rename;
  return TRUE;
}

/* Every crash point the engine (or a tool start) reaches: a kill there kills the engine; a stop there holds it until the
 * stop has begun. Any thread. */
static gboolean
run_point (const char *point, gpointer data, BroBroError **error)
{
  BroScenarioRun *run = data;
  gboolean kill, hold;
  if (g_str_has_prefix (point, "beforeStep:"))
    running (run, point + 11);
  else if (g_str_has_prefix (point, "afterStep:"))
    running (run, NULL);
  g_mutex_lock (&run->lock);
  kill = run->kill_at && !run->killed && same_point (point, run->kill_at);
  hold = !kill && run->hold_at && same_point (point, run->hold_at);
  if (kill)
    run->killed = g_strdup (point);
  if (hold)
    g_clear_pointer (&run->hold_at, g_free);
  g_mutex_unlock (&run->lock);
  if (kill)
    {
      BroBroError *thrown;
      bro_scenario_tools_end_all (run->tools);
      thrown = test_composition_root_kill (run->root, point);
      g_mutex_lock (&run->lock);
      run->has_cutoff = TRUE;
      run->cutoff = run->root->killed_seq;
      g_mutex_unlock (&run->lock);
      if (error)
        *error = thrown;
      else
        bro_bro_error_free (thrown);
      return FALSE;
    }
  if (hold)
    {
      gint64 until = g_get_monotonic_time () + 60 * G_USEC_PER_SEC;
      g_mutex_lock (&run->lock);
      run->held = TRUE;
      g_cond_broadcast (&run->cond);
      while (!run->released && g_cond_wait_until (&run->cond, &run->lock, until))
        ;
      g_mutex_unlock (&run->lock);
    }
  return TRUE;
}

/* Until the engine has died at the armed kill point (the engine runs, settling, until then); a point it never reaches
 * fails the scenario. */
static gboolean
killed (BroScenarioRun *run)
{
  g_autoptr (BroJsonValue) jobs = NULL;
  g_autoptr (GPtrArray) calls = NULL;
  g_autoptr (GString) found = NULL;
  g_autofree char *expected = NULL;
  if (killed_yet (run))
    return TRUE;
  if (!bro_scenario_run_settle (run))
    return FALSE;
  if (killed_yet (run))
    return TRUE;
  expected = g_strconcat ("the engine to reach ", run->kill_at, NULL);
  found = g_string_new ("it never did; jobs ");
  if ((jobs = bro_scenario_run_all_jobs (run)))
    for (guint i = 0; i < bro_json_value_length (jobs); i++)
      {
        BroJsonValue *j = bro_json_value_at (jobs, i);
        g_autofree char *error = bro_scenario_json_text (J (j, "error"));
        g_string_append_printf (found, "%s%s %s %s %s", i ? ", " : "", bro_scenario_run_name_of (run, S (j, "id", "")), S (j, "state", ""),
                                S (j, "outcome", ""), error);
      }
  calls = bro_scenario_tools_calls (run->tools, "makemkvcon");
  g_string_append (found, "; makemkvcon ");
  for (guint i = 0; i < calls->len; i++)
    g_string_append_printf (found, "%s%s", i ? " | " : "", (char *) calls->pdata[i]);
  return bro_scenario_run_fail (run, expected, found->str);
}

static void
barrier_reached (gpointer data)
{
  test_waiter_finish (data, NULL, NULL, NULL);
}

/* H1-6: the engine is held at the point, LocalEngine's stop begins, the point is released, the stop ends. A point never
 * reached, or a stop that hits the 30 s cap, fails the scenario. */
static gboolean
stop_at (BroScenarioRun *run, const char *point)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) (BRO_SCENARIO_PATIENCE * G_USEC_PER_SEC), began;
  g_autoptr (TestWaiter) stopping = NULL;
  TestWaiter *reached;
  gboolean held = FALSE;
  for (;;)
    {
      g_mutex_lock (&run->lock);
      held = run->held;
      g_mutex_unlock (&run->lock);
      if (held || g_get_monotonic_time () > deadline)
        break;
      g_usleep (5000);
      if (bro_engine_executor_activity (run->root->executor) == bro_scenario_tools_parked (run->tools))
        {
          g_autoptr (BroJsonValue) jobs = bro_scenario_run_all_jobs (run);
          gboolean all = jobs != NULL;
          for (guint i = 0; jobs && i < bro_json_value_length (jobs); i++)
            {
              const char *state = S (bro_json_value_at (jobs, i), "state", "");
              all &= !strcmp (state, "finished") || !strcmp (state, "blocked") || !strcmp (state, "awaitingDecision");
            }
          g_mutex_lock (&run->lock);
          held = run->held;
          g_mutex_unlock (&run->lock);
          if (all && !held)
            break;
        }
    }
  if (!held)
    {
      g_autofree char *expected = g_strconcat ("the engine to reach ", point, NULL);
      g_mutex_lock (&run->lock);
      g_clear_pointer (&run->hold_at, g_free);
      g_mutex_unlock (&run->lock);
      return bro_scenario_run_fail (run, expected, "it never did");
    }
  began = g_get_monotonic_time ();
  stopping = test_composition_root_stop (run->root);
  /* An executor barrier, at most 200 ms (a point that holds the executor never lets it through). */
  reached = test_waiter_new ();
  if (bro_engine_executor_post (run->root->executor, barrier_reached, test_waiter_ref (reached), test_waiter_unref))
    test_waiter_wait (reached, 0.2);
  test_waiter_unref (reached);
  g_mutex_lock (&run->lock);
  run->released = TRUE;
  g_cond_broadcast (&run->cond);
  g_mutex_unlock (&run->lock);
  if (!test_waiter_wait (stopping, BRO_SCENARIO_STOP_PATIENCE))
    return bro_scenario_run_fail (run, "the stop to end within 30 s", "it never ended");
  run->root->last_stop = (double) (g_get_monotonic_time () - began) / G_USEC_PER_SEC;
  drop_feed (run);
  if (run->root->last_stop >= TEST_ROOT_STOP_CAP)
    return bro_scenario_run_failf (run, "the stop to end within 30 s", "%.1f s (the cap)", run->root->last_stop);
  return TRUE;
}

/* After a kill: once the point was reached, the killed engine is finished off and a new one starts on the same database
 * and files. Otherwise the engine settles, stops (a stop that hits the 30 s cap fails) and a new one starts. The feed
 * reopens after the last seq the old bus had. */
static gboolean
restart (BroScenarioRun *run)
{
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *why = NULL;
  gboolean has_feed;
  gint64 after;
  g_mutex_lock (&run->lock);
  has_feed = run->drain != NULL;
  g_mutex_unlock (&run->lock);
  if (run->kill_at)
    {
      if (!killed (run))
        return FALSE;
    }
  else if (has_feed && !bro_scenario_run_settle (run))
    return FALSE;
  catch_up (run);
  after = bro_event_bus_last_seq (run->root->bus);
  close_feed (run);
  if (!test_composition_root_restart (run->root, BRO_SCENARIO_STOP_PATIENCE, &error, &why))
    {
      g_autoptr (BroJsonValue) e = error ? bro_scenario_error_json (error) : NULL;
      g_autofree char *text = e ? bro_scenario_json_text (e) : NULL;
      return bro_scenario_run_failf (run, "restart to succeed", "%s", why ? why : text ? text : "it never ended");
    }
  g_mutex_lock (&run->lock);
  g_clear_pointer (&run->kill_at, g_free);
  g_clear_pointer (&run->killed, g_free);
  run->held = FALSE;
  run->released = FALSE;
  g_mutex_unlock (&run->lock);
  running (run, NULL);
  if (!open_feed (run, TRUE, after))
    return FALSE;
  arm (run, (guint) run->step_number);
  return TRUE;
}

/* ---- the steps ---- */

/* H1-1: chosen titles as SessionService will store them: the disc's listing and every title ticked or not. */
static BroSessionSnapshot *
snapshot_of (BroJsonValue *disc, BroJsonValue *titles)
{
  BroListing *listing = bro_scenario_parsed_listing (disc);
  BroSessionChoices *choices = bro_session_choices_new ();
  for (guint i = 0; i < listing->titles->len; i++)
    {
      gboolean ticked = FALSE;
      for (guint k = 0; k < bro_json_value_length (titles); k++)
        ticked |= bro_json_value_get_integer (bro_json_value_at (titles, k), -1) == (gint64) i;
      g_ptr_array_add (choices->titles, bro_title_choice_new ((int) i, ticked, NULL, NULL));
    }
  return bro_session_snapshot_new (listing, choices);
}

static gboolean
start_job (BroScenarioRun *run, BroJobRequest *request, BroJsonValue **job)
{
  *job = CALL (run, bro_local_engine_start_job (run->root->engine, request, job_done, d, test_waiter_unref));
  return *job != NULL;
}

/* Decision 238: an ISO, folder or file through startJob (chosen titles as a session snapshot); a drive through rip, or
 * startJob when the rip is automatic or has titles. */
static gboolean
rip (BroScenarioRun *run, BroJsonValue *v)
{
  g_autoptr (BroJsonValue) options = bro_json_value_new_object ();
  g_autoptr (BroJsonValue) job = NULL;
  BroJsonValue *titles = J (v, "titles");
  gboolean automatic = bro_json_value_get_bool (J (v, "automatic"), FALSE);
  const char *as = S (v, "as", NULL);
  if (J (v, "session"))
    return bro_scenario_run_fail (run, "rip.session (SessionService, phase 4)", "not built");
  if (J (v, "tracks"))
    return bro_scenario_run_fail (run, "rip.tracks (Transform, phase 5)", "not built");
  if (S (v, "mode", NULL))
    bro_json_value_set (options, "mode", bro_json_value_new_string (S (v, "mode", "")));
  if (S (v, "profile", NULL))
    bro_json_value_set (options, "profile", bro_json_value_new_string (S (v, "profile", "")));
  if (S (v, "outputFolder", NULL))
    {
      g_autofree char *path = fill_root (S (v, "outputFolder", ""));
      g_autoptr (BroJsonValue) libraries = CALL (run, bro_engine_api_libraries (e, libraries_done, d, test_waiter_unref));
      const char *library = NULL;
      g_autofree char *want = g_strdup (path);
      gsize n = strlen (want);
      while (n > 0 && want[n - 1] == '/')
        want[--n] = '\0';
      if (!libraries)
        return FALSE;
      for (guint i = 0; i < bro_json_value_length (libraries) && !library; i++)
        {
          g_autofree char *p = g_strdup (S (bro_json_value_at (libraries, i), "path", ""));
          gsize m = strlen (p);
          while (m > 0 && p[m - 1] == '/')
            p[--m] = '\0';
          if (!strcmp (p, want))
            library = S (bro_json_value_at (libraries, i), "id", "");
        }
      if (!library)
        {
          g_autoptr (BroJsonValue) e = bro_json_value_new_object ();
          BroJsonValue *params = bro_json_value_new_object ();
          g_autofree char *text = NULL;
          bro_json_value_set (e, "code", bro_json_value_new_string (bro_message_code_wire (BRO_MSG_LIBRARY_NOT_ATTACHED)));
          bro_json_value_set (params, "path", bro_json_value_new_string (path));
          bro_json_value_set (e, "params", params);
          text = bro_scenario_json_text (e);
          return bro_scenario_run_failf (run, "rip to succeed", "error %s", text);
        }
      bro_json_value_set (options, "library", bro_json_value_new_string (library));
    }
  if (S (v, "drive", NULL))
    {
      const char *drive_name = S (v, "drive", "");
      g_autofree char *drive_id = bro_scenario_run_drive_id (run, drive_name);
      if (!drive_id)
        return FALSE;
      if (!automatic && !titles)
        {
          g_autoptr (BroRipOptionsDto) o = bro_rip_options_dto_new ();
          o->has_mode = S (v, "mode", NULL) && bro_rip_mode_from_wire (S (v, "mode", ""), &o->mode);
          o->profile = g_strdup (S (v, "profile", NULL));
          o->output_folder = S (v, "outputFolder", NULL) ? fill_root (S (v, "outputFolder", "")) : NULL;
          if (!(job = CALL (run, bro_engine_api_rip (e, drive_id, o, job_done, d, test_waiter_unref))))
            return FALSE;
        }
      else
        {
          g_autoptr (BroJsonValue) drives = bro_scenario_engine_drives (run);
          BroJsonValue *drive = NULL, *medium, *generation;
          BroScenarioDrive *world_drive = bro_scenario_world_drive_named (run->world, drive_name, &run->failure);
          g_autoptr (BroJobRequest) request = NULL;
          g_autoptr (BroJsonValue) disc = NULL;
          if (!drives || !world_drive)
            return FALSE;
          for (guint i = 0; i < bro_json_value_length (drives); i++)
            if (!g_strcmp0 (S (bro_json_value_at (drives, i), "id", NULL), drive_id))
              drive = bro_json_value_at (drives, i);
          g_mutex_lock (&run->world->lock);
          disc = world_drive->disc ? bro_json_value_ref (world_drive->disc) : NULL;
          g_mutex_unlock (&run->world->lock);
          medium = J (drive, "medium");
          request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, S (medium, "label", disc ? S (disc, "volume", S (drive, "name", "")) : S (drive, "name", "")));
          request->automatic = automatic;
          request->drive_id = g_strdup (drive_id);
          generation = J (medium, "generation");
          request->has_media_generation = generation && generation->kind == BRO_JSON_VALUE_INTEGER;
          request->media_generation = bro_json_value_get_integer (generation, 0);
          request->options = bro_json_value_ref (options);
          if (titles && disc)
            {
              g_autoptr (BroDiscRequest) details = bro_disc_request_new (bro_disc_source_new (BRO_DISC_SOURCE_DRIVE, drive_id), snapshot_of (disc, titles));
              request->details = bro_disc_request_encode (details);
            }
          if (!start_job (run, request, &job))
            return FALSE;
        }
    }
  else
    {
      g_autoptr (BroJsonValue) disc = bro_scenario_world_disc (run->world);
      g_autoptr (BroJobRequest) request = NULL;
      g_autoptr (BroDiscRequest) details = NULL;
      BroDiscSource *source;
      if (J (v, "iso"))
        source = bro_disc_source_new (BRO_DISC_SOURCE_ISO, BRO_SCENARIO_ISO);
      else if (S (v, "folder", NULL))
        {
          g_autofree char *path = bro_scenario_path (S (v, "folder", ""));
          source = bro_disc_source_new (BRO_DISC_SOURCE_FOLDER, path);
        }
      else if (S (v, "file", NULL))
        {
          g_autofree char *path = bro_scenario_path (S (v, "file", ""));
          source = bro_disc_source_new (BRO_DISC_SOURCE_FILE, path);
        }
      else
        {
          g_autofree char *text = bro_scenario_json_text (v);
          return bro_scenario_run_fail (run, "rip.iso, drive, folder or file", text);
        }
      if (!disc)
        {
          bro_disc_source_free (source);
          return bro_scenario_run_fail (run, "given.disc for rip", "none");
        }
      request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, S (disc, "volume", ""));
      request->automatic = automatic;
      request->options = bro_json_value_ref (options);
      details = bro_disc_request_new (source, titles ? snapshot_of (disc, titles) : NULL);
      request->details = bro_disc_request_encode (details);
      if (!start_job (run, request, &job))
        return FALSE;
    }
  if (as)
    name_job (run, as, S (job, "id", ""));
  else
    {
      g_autofree char *name = g_strdup_printf ("job%u", g_hash_table_size (run->jobs) + 1);
      name_job (run, name, S (job, "id", ""));
    }
  return TRUE;
}

static char *
library_path (BroScenarioRun *run, const char *id)
{
  g_autoptr (BroJsonValue) config = bro_scenario_world_configuration (run->world);
  BroJsonValue *libraries = J (config, "libraries");
  for (guint i = 0; i < bro_json_value_length (libraries); i++)
    if (!g_strcmp0 (S (bro_json_value_at (libraries, i), "id", NULL), id))
      return g_strdup (S (bro_json_value_at (libraries, i), "path", ""));
  return bro_scenario_path (id);
}

static gboolean
decide (BroScenarioRun *run, BroJsonValue *v)
{
  BroId id;
  g_autoptr (BroJsonValue) detail = NULL;
  g_autoptr (BroJsonValue) again = NULL;
  BroJsonValue *decision;
  const char *name = S (v, "job", "");
  if (!bro_scenario_run_job (run, name, &id) || !(detail = bro_scenario_engine_job (run, &id)))
    return FALSE;
  /* The question may not be asked yet (the job was just started): wait for it, as wait would. */
  if (!J (detail, "decision"))
    {
      g_autoptr (BroJsonValue) when = bro_json_value_new_object ();
      bro_json_value_set (when, "job", bro_json_value_new_string (name));
      bro_json_value_set (when, "state", bro_json_value_new_string ("awaitingDecision"));
      if (!bro_scenario_expect_when (run, when))
        return FALSE;
    }
  if (!(again = bro_scenario_engine_job (run, &id)))
    return FALSE;
  if (!(decision = J (again, "decision")))
    {
      g_autofree char *what = g_strdup_printf ("job %s awaiting a decision", name);
      return bro_scenario_run_fail (run, what, "none");
    }
  return done_ok (run, CALL (run, bro_engine_api_decide (e, &id, S (decision, "id", ""), S (v, "answer", ""), test_waiter_done, d,
                                                         test_waiter_unref)));
}

static gboolean
step (BroScenarioRun *run, const char *kind, BroJsonValue *v)
{
  BroId id;
  if (!strcmp (kind, "expect"))
    return bro_scenario_expect_check (run, v);
  if (!strcmp (kind, "wait"))
    {
      g_autoptr (BroJsonValue) e = bro_json_value_new_object ();
      bro_json_value_set (e, "when", bro_json_value_ref (v));
      return bro_scenario_expect_check (run, e);
    }
  if (!strcmp (kind, "rip"))
    return rip (run, v);
  if (!strcmp (kind, "insert"))
    {
      g_autoptr (BroJsonValue) disc = J (v, "disc") ? bro_json_value_ref (J (v, "disc")) : bro_scenario_world_disc (run->world);
      if (!disc)
        return bro_scenario_run_fail (run, "a disc to insert", "none");
      /* The engine has handled the OS's event (its drive scan) before the next step. */
      return bro_scenario_world_insert (run->world, S (v, "drive", ""), disc, &run->failure) && quiet (run);
    }
  if (!strcmp (kind, "remove"))
    return bro_scenario_world_remove (run->world, S (v, "drive", ""), &run->failure) && quiet (run);
  if (!strcmp (kind, "openTray"))
    return bro_scenario_world_open_tray (run->world, S (v, "drive", ""), &run->failure) && quiet (run);
  if (!strcmp (kind, "eject") || !strcmp (kind, "closeTray"))
    {
      g_autofree char *drive = bro_scenario_run_drive_id (run, S (v, "drive", ""));
      gboolean eject = !strcmp (kind, "eject");
      if (!drive)
        return FALSE;
      return done_ok (run, CALL (run, eject ? bro_engine_api_eject (e, drive, test_waiter_done, d, test_waiter_unref)
                                            : bro_engine_api_close_tray (e, drive, test_waiter_done, d, test_waiter_unref)));
    }
  if (!strcmp (kind, "keepApart"))
    return bro_scenario_run_job (run, bro_json_value_get_string (v, ""), &id)
           && done_ok (run, CALL (run, bro_engine_api_keep_apart (e, &id, test_waiter_done, d, test_waiter_unref)));
  if (!strcmp (kind, "cancel"))
    return bro_scenario_run_job (run, bro_json_value_get_string (v, ""), &id)
           && done_ok (run, CALL (run, bro_engine_api_cancel (e, &id, test_waiter_done, d, test_waiter_unref)));
  if (!strcmp (kind, "decide"))
    return decide (run, v);
  if (!strcmp (kind, "move"))
    return bro_scenario_run_job (run, S (v, "job", ""), &id)
           && done_ok (run, CALL (run, bro_engine_api_move (e, &id, (int) bro_json_value_get_integer (J (v, "by"), 0), test_waiter_done, d,
                                                          test_waiter_unref)));
  if (!strcmp (kind, "startNow"))
    return bro_scenario_run_job (run, bro_json_value_get_string (v, ""), &id)
           && done_ok (run, CALL (run, bro_engine_api_start_now (e, &id, test_waiter_done, d, test_waiter_unref)));
  if (!strcmp (kind, "retry"))
    {
      g_autoptr (BroJsonValue) job = NULL;
      BroStepKind from;
      gboolean has_from = S (v, "fromStep", NULL) && bro_step_kind_from_wire (S (v, "fromStep", ""), &from);
      if (J (v, "drive") || J (v, "titles") || J (v, "makemkvSettings"))
        {
          g_autofree char *text = bro_scenario_json_text (v);
          return bro_scenario_run_fail (run, "retry with only job, fromStep and as (phase 3)", text);
        }
      if (!bro_scenario_run_job (run, S (v, "job", ""), &id)
          || !(job = CALL (run, bro_engine_api_retry (e, &id, has_from ? &from : NULL, job_done, d, test_waiter_unref))))
        return FALSE;
      if (S (v, "as", NULL))
        name_job (run, S (v, "as", ""), S (job, "id", ""));
      return TRUE;
    }
  if (!strcmp (kind, "advance"))
    return advance (run, (gint64) (bro_json_value_get_number (v, 0) * 1000 + 0.5));
  if (!strcmp (kind, "kill"))
    return killed (run); /* armed since the start or the last restart (arm) */
  if (!strcmp (kind, "stop"))
    return stop_at (run, S (v, "at", ""));
  if (!strcmp (kind, "restart"))
    return restart (run);
  if (!strcmp (kind, "unmount") || !strcmp (kind, "mount"))
    {
      g_autofree char *path = library_path (run, bro_json_value_get_string (v, ""));
      gboolean ok = !strcmp (kind, "mount") ? test_composition_root_mount (run->root, path, BRO_SCENARIO_PATIENCE)
                                            : test_composition_root_unmount (run->root, path, BRO_SCENARIO_PATIENCE);
      return ok || bro_scenario_run_failf (run, "the library check to end", "it never did");
    }
  if (!strcmp (kind, "attachLibrary"))
    {
      g_autofree char *path = fill_root (S (v, "path", ""));
      return done_ok (run, CALL (run, bro_engine_api_attach_library (e, path, S (v, "name", ""), library_done, d, test_waiter_unref)));
    }
  if (!strcmp (kind, "config"))
    {
      g_autoptr (BroJsonValue) envelope = bro_scenario_engine_config (run);
      g_autoptr (BroJsonValue) merged = NULL;
      g_autoptr (BroJsonValue) filled = NULL;
      if (!envelope)
        return FALSE;
      merged = bro_scenario_merge_patch (J (envelope, "doc"), v);
      filled = bro_scenario_fill_root (merged);
      return done_ok (run, CALL (run, bro_engine_api_update_config (e, filled, bro_json_value_get_integer (J (envelope, "version"), 0),
                                                                   envelope_done, d, test_waiter_unref)));
    }
  if (!strcmp (kind, "tools"))
    {
      for (guint i = 0; v->kind == BRO_JSON_VALUE_OBJECT && i < v->keys->len; i++)
        bro_scenario_world_set_script (run->world, v->keys->pdata[i], v->items->pdata[i]);
      return TRUE;
    }
  if (!strcmp (kind, "write"))
    {
      g_autofree char *path = bro_scenario_path (S (v, "path", ""));
      g_autoptr (GBytes) bytes = bro_scenario_file_bytes (J (v, "text") ? J (v, "text") : J (v, "bytes"));
      test_memory_fs_put (run->root->fs, path, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
      return TRUE;
    }
  if (!strcmp (kind, "flip"))
    {
      g_autofree char *path = bro_scenario_path (S (v, "path", ""));
      g_autoptr (BroBroError) error = NULL;
      g_autoptr (GBytes) bytes = bro_file_system_read (BRO_FILE_SYSTEM (run->root->fs), path, &error);
      gsize n, at = (gsize) bro_json_value_get_integer (J (v, "byte"), 0);
      guint8 *copy;
      if (!bytes)
        return bro_scenario_run_fail (run, path, "none");
      copy = g_memdup2 (g_bytes_get_data (bytes, &n), MAX (n, 1));
      if (at < n)
        copy[at] ^= 1;
      test_memory_fs_put (run->root->fs, path, copy, n);
      g_free (copy);
      return TRUE;
    }
  if (!strcmp (kind, "delete"))
    {
      g_autofree char *path = bro_scenario_path (bro_json_value_get_string (v, ""));
      g_autoptr (BroBroError) error = NULL;
      return bro_file_system_remove (BRO_FILE_SYSTEM (run->root->fs), path, &error) || bro_scenario_run_fail (run, "delete to succeed", path);
    }
  if (!strcmp (kind, "swapDisc"))
    {
      g_autoptr (BroJsonValue) disc = bro_scenario_catalog_disc (v);
      bro_scenario_world_set_disc (run->world, disc);
      return TRUE;
    }
  if (!strcmp (kind, "open") || !strcmp (kind, "choose"))
    {
      g_autofree char *what = g_strdup_printf ("%s (SessionService, phase 4)", kind);
      return bro_scenario_run_fail (run, what, "not built");
    }
  if (!strcmp (kind, "cancelCountdown") || !strcmp (kind, "mounted"))
    {
      g_autofree char *what = g_strdup_printf ("%s (AutomationService, phase 4)", kind);
      return bro_scenario_run_fail (run, what, "not built");
    }
  if (!strcmp (kind, "api"))
    return bro_scenario_run_fail (run, "api (the Server, phase 4)", "not built");
  if (!strcmp (kind, "verify") || !strcmp (kind, "rescan") || !strcmp (kind, "testStep") || !strcmp (kind, "testNotification"))
    {
      g_autofree char *what = g_strdup_printf ("%s (phase 5)", kind);
      return bro_scenario_run_fail (run, what, "not built");
    }
  if (!strcmp (kind, "repair"))
    return bro_scenario_run_fail (run, "repair (phase 7)", "not built");
  return bro_scenario_run_fail (run, "a known step", kind);
}

/* ---- the deferred launcher ---- */

/* The root takes its launcher before the tools exist (they need its clock and files): this hands each start on. */
#define TYPE_DEFERRED (scenario_deferred_get_type ())
G_DECLARE_FINAL_TYPE (ScenarioDeferred, scenario_deferred, SCENARIO, DEFERRED, GObject)

struct _ScenarioDeferred {
  GObject parent_instance;
  BroScenarioRun *run;
};

static BroRunningProcess *
deferred_start (BroProcessLauncher *launcher, const BroProcessSpec *spec, BroBroError **error)
{
  return bro_process_launcher_start (BRO_PROCESS_LAUNCHER (SCENARIO_DEFERRED (launcher)->run->tools), spec, error);
}

static void
deferred_iface_init (BroProcessLauncherInterface *iface)
{
  iface->start = deferred_start;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (ScenarioDeferred, scenario_deferred, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_PROCESS_LAUNCHER, deferred_iface_init))

static void scenario_deferred_class_init (ScenarioDeferredClass *klass) {}
static void scenario_deferred_init (ScenarioDeferred *self) {}

/* ---- the run ---- */

static BroScenarioRun *
run_new (const char *id, BroJsonValue *scenario)
{
  BroScenarioRun *run = g_new0 (BroScenarioRun, 1);
  g_autoptr (ScenarioDeferred) deferred = g_object_new (TYPE_DEFERRED, NULL);
  run->id = g_strdup (id);
  run->scenario = bro_json_value_ref (scenario);
  run->jobs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  run->job_names = g_ptr_array_new_with_free_func (g_free);
  run->events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_json_value_unref);
  run->step_kind = g_strdup ("start");
  g_mutex_init (&run->lock);
  g_cond_init (&run->cond);
  g_mutex_init (&run->step_lock);
  run->faults = g_ptr_array_new_with_free_func ((GDestroyNotify) fault_free);
  run->world = bro_scenario_world_new (scenario);
  deferred->run = run;
  run->root = test_composition_root_new (bro_scenario_world_installed (run->world, "makemkvcon"), NULL,
                                         bro_scenario_world_installed (run->world, "mkvmerge"), NULL, 8, 1024, BRO_PROCESS_LAUNCHER (deferred));
  run->tools = bro_scenario_tools_new (run->world, run->root->clock, run->root->fs);
  run->root->crash = run_point;
  run->root->crash_data = run;
  bro_scenario_tools_set_crash (run->tools, run_point, run);
  return run;
}

/* Stops like the engine: a held point is released, the feed closes, the root stops (or finishes a kill) and is freed, and
 * the clock's timers go. */
static void
run_free (BroScenarioRun *run)
{
  g_autoptr (TestManualClock) clock = g_object_ref (run->root->clock);
  g_mutex_lock (&run->lock);
  run->released = TRUE;
  g_cond_broadcast (&run->cond);
  g_mutex_unlock (&run->lock);
  close_feed (run);
  if (!run->root->killed)
    bro_scenario_tools_end_all (run->tools); /* every process ends, so the stop doesn't wait out a grace */
  test_composition_root_free (run->root);
  /* The clock keeps a cancelled timer until it moves past it, and a process's timer holds the process, which holds the
   * tools, which hold the clock: drop them all. */
  test_manual_clock_cancel_all (clock);
  g_object_unref (run->tools);
  bro_scenario_world_free (run->world);
  g_hash_table_unref (run->jobs);
  g_ptr_array_unref (run->job_names);
  g_ptr_array_unref (run->events);
  bro_json_value_unref (run->scenario);
  g_free (run->id);
  g_free (run->kill_at);
  g_free (run->killed);
  g_free (run->hold_at);
  g_free (run->step_kind);
  g_free (run->failure);
  g_free (run->running);
  g_ptr_array_unref (run->faults);
  g_mutex_clear (&run->step_lock);
  g_mutex_clear (&run->lock);
  g_cond_clear (&run->cond);
  g_free (run);
}

static gboolean
play_all (BroScenarioRun *run)
{
  BroJsonValue *steps = J (run->scenario, "steps");
  run->step_number = -1; /* the given */
  if (!bro_scenario_world_prepare (run->world, run->root, &run->failure) || !arm_faults (run))
    return FALSE;
  run->step_number = 0;
  {
    g_autoptr (TestWaiter) started = test_composition_root_start (run->root);
    if (!test_waiter_wait (started, BRO_SCENARIO_PATIENCE) || started->error)
      return bro_scenario_run_fail (run, "the engine starts", started->error ? started->error->code : "it never did");
  }
  if (!open_feed (run, FALSE, 0))
    return FALSE;
  arm (run, 0);
  for (guint i = 0; i < bro_json_value_length (steps); i++)
    {
      BroJsonValue *s = bro_json_value_at (steps, i);
      run->step_number = (int) i + 1;
      g_free (run->step_kind);
      run->step_kind = g_strdup (s->keys->len ? (char *) s->keys->pdata[0] : "?");
      if (!step (run, run->step_kind, s->items->len ? s->items->pdata[0] : NULL))
        {
          if (!run->failure)
            bro_scenario_run_failf (run, "the step to hold", "it failed without saying why");
          return FALSE;
        }
    }
  if (run->kill_at)
    {
      g_autofree char *expected = g_strdup_printf ("the kill at %s to be followed by a restart", run->kill_at);
      return bro_scenario_run_fail (run, expected, "the end of the steps");
    }
  return TRUE;
}

gboolean
bro_scenario_run_play (const char *id, BroJsonValue *scenario, char **failure)
{
  BroScenarioRun *run = run_new (id, scenario);
  gboolean ok = play_all (run);
  if (!ok && failure)
    *failure = run->step_number < 0 ? g_strdup_printf ("%s given: %s", id, run->failure ? run->failure : "?")
                                     : g_strdup_printf ("%s step %d (%s): %s", id, run->step_number, run->step_kind, run->failure ? run->failure : "?");
  run_free (run);
  return ok;
}
