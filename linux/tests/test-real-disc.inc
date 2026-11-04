/* test-real-disc.inc: the realDisc shared scenarios (decisions 302–304) through the real root: real makemkvcon and mkvmerge,
 * real disk, the image only read. Every scenario whose phase is built is a test case (/real-disc/<id>, a suite of its own:
 * GTest's -p takes one suite's cases): skipped with the reason when its variable isn't set, and a failure is a failure when
 * it is. Nothing of the user's is read: a scratch world, an empty
 * home, PATH alone from the environment, no system keystore, no drives. Exactly what the phase 3 real-disc scenarios use is
 * supported; anything else fails as "not supported in a real world: <key>", so a later one can't pass half-read. */
#include "bro-disc-request.h"
#include "bro-job-detail-dto.h"
#include "bro-job-page-dto.h"
#include "bro-job-query-dto.h"
#include "bro-job-request.h"
#include "bro-library-marker.h"
#include "bro-log-line-dto.h"
#include "bro-seal-step.h"
#include "bro-sha256-hasher.h"
#include "bro-sha256-sums.h"
#include "bro-sum-entry.h"
#include "bro-test-fixtures.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <sys/statvfs.h>

#define REAL_BUILT_PHASE 3

#define J bro_json_value_member
#define S(v, key, fallback) bro_json_value_get_string (J ((v), (key)), (fallback))

/* shared/scenarios/json, from the fixtures folder. */
static char *
real_folder (void)
{
  g_autofree char *fixtures = bro_test_fixture_path ("");
  return g_build_filename (fixtures, "..", "scenarios", "json", NULL);
}

static BroJsonValue *
real_load (const char *id)
{
  g_autofree char *folder = real_folder ();
  g_autofree char *name = g_strconcat (id, ".json", NULL);
  g_autofree char *path = g_build_filename (folder, name, NULL);
  g_autofree char *text = NULL;
  gsize length;
  if (!g_file_get_contents (path, &text, &length, NULL))
    return NULL;
  return bro_json_value_parse (text, length);
}

static gint
real_by_name (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(char *const *) a, *(char *const *) b);
}

/* The ids of the scenarios with realDisc whose phase is built, sorted. */
static GPtrArray *
real_selected (void)
{
  GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *folder = real_folder ();
  g_autoptr (GDir) dir = g_dir_open (folder, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir)))
    if (g_str_has_suffix (name, ".json"))
      {
        g_autofree char *id = g_strndup (name, strlen (name) - 5);
        g_autoptr (BroJsonValue) s = real_load (id);
        if (s && J (s, "realDisc") && bro_json_value_get_integer (J (s, "phase"), 99) <= REAL_BUILT_PHASE)
          g_ptr_array_add (ids, g_steal_pointer (&id));
      }
  g_ptr_array_sort (ids, real_by_name);
  return ids;
}

/* One scenario through the real root (decision 302): a fresh world under the temporary folder (one volume, so staging →
 * library is a rename), removed afterwards; on a failure the job's logs and transcripts are copied out first. */
typedef struct {
  const char *id;
  BroJsonValue *scenario;
  const char *iso;
  char *root, *data, *home, *library;
  GPtrArray *names; /* char *: the jobs' names */
  GPtrArray *ids;   /* char *: their ids */
  BroLocalEngine *engine;
  int step;
  const char *kind;
  char *failure;
  gint64 started;
} RealRun;

/* FALSE, with the first failure kept as "<id> step <n> (<kind>): <expected> / <found>". */
static gboolean
real_fail (RealRun *run, const char *expected, const char *found)
{
  if (!run->failure)
    run->failure = g_strdup_printf ("%s step %d (%s): %s%s%s", run->id, run->step, run->kind, expected, found ? " / " : "", found ? found : "");
  return FALSE;
}

static gboolean G_GNUC_PRINTF (3, 4)
real_failf (RealRun *run, const char *expected, const char *format, ...)
{
  va_list args;
  g_autofree char *found = NULL;
  va_start (args, format);
  found = g_strdup_vprintf (format, args);
  va_end (args);
  return real_fail (run, expected, found);
}

static char *
real_text (const BroJsonValue *v)
{
  g_autoptr (GBytes) bytes = NULL;
  gsize size;
  const char *data;
  if (!v)
    return g_strdup ("null");
  bytes = bro_json_value_encode_canonical (v);
  data = g_bytes_get_data (bytes, &size);
  return g_strndup (data, size);
}

/* ---- what a real world supports ---- */

static gboolean
real_only (RealRun *run, const char *where, const BroJsonValue *value, const char *const *keys)
{
  if (!value || value->kind != BRO_JSON_VALUE_OBJECT)
    return TRUE;
  for (guint i = 0; i < value->keys->len; i++)
    if (!g_strv_contains (keys, value->keys->pdata[i]))
      {
        g_autofree char *what = g_strconcat ("not supported in a real world: ", where, value->keys->pdata[i], NULL);
        return real_fail (run, what, NULL);
      }
  return TRUE;
}

#define ONLY(run, where, value, ...) real_only ((run), (where), (value), (const char *const[]) { __VA_ARGS__, NULL })

static gboolean
real_check_supported (RealRun *run)
{
  BroJsonValue *steps = J (run->scenario, "steps");
  if (!ONLY (run, "", run->scenario, "title", "phase", "realDisc", "from", "given", "steps")
      || !ONLY (run, "given.", J (run->scenario, "given"), "profile"))
    return FALSE;
  for (guint i = 0; i < bro_json_value_length (steps); i++)
    {
      BroJsonValue *step = bro_json_value_at (steps, i);
      const char *kind = step->keys->pdata[0];
      BroJsonValue *v = step->items->pdata[0];
      run->step = (int) i + 1;
      run->kind = kind;
      if (g_str_equal (kind, "rip"))
        {
          if (!ONLY (run, "rip.", v, "iso", "as"))
            return FALSE;
        }
      else if (g_str_equal (kind, "expect"))
        {
          BroJsonValue *calls = J (v, "calls");
          if (!ONLY (run, "expect.", v, "job", "jobs", "log", "calls", "files", "sums")
              || !ONLY (run, "expect.job.", J (v, "job"), "name", "state", "outcome", "error", "noError", "files")
              || !ONLY (run, "expect.jobs.", J (v, "jobs"), "count", "outcomes", "states", "kinds")
              || !ONLY (run, "expect.log.", J (v, "log"), "job", "include", "exclude")
              || !ONLY (run, "expect.calls.", calls, "makemkvcon")
              || !ONLY (run, "expect.calls.makemkvcon.", calls ? J (calls, "makemkvcon") : NULL, "none", "count", "include", "exclude", "counts")
              || !ONLY (run, "expect.files.", J (v, "files"), "under", "exactly", "include", "exclude")
              || !ONLY (run, "expect.sums.", J (v, "sums"), "folder", "ok", "files"))
            return FALSE;
        }
      else
        {
          g_autofree char *what = g_strconcat ("not supported in a real world: ", kind, NULL);
          return real_fail (run, what, NULL);
        }
    }
  run->step = 0;
  run->kind = "start";
  return TRUE;
}

/* ---- the world ---- */

/* RFC 7386. */
static BroJsonValue *
real_merge_patch (BroJsonValue *target, BroJsonValue *patch)
{
  BroJsonValue *out;
  if (!patch || patch->kind != BRO_JSON_VALUE_OBJECT)
    return bro_json_value_ref (patch);
  out = bro_json_value_new_object ();
  if (target && target->kind == BRO_JSON_VALUE_OBJECT)
    for (guint i = 0; i < target->keys->len; i++)
      {
        BroJsonValue *p = J (patch, target->keys->pdata[i]);
        if (!p)
          bro_json_value_set (out, target->keys->pdata[i], bro_json_value_ref (target->items->pdata[i]));
        else if (p->kind != BRO_JSON_VALUE_NULL)
          bro_json_value_set (out, target->keys->pdata[i], real_merge_patch (target->items->pdata[i], p));
      }
  for (guint i = 0; i < patch->keys->len; i++)
    {
      BroJsonValue *p = patch->items->pdata[i];
      if (!(target && J (target, patch->keys->pdata[i])) && p->kind != BRO_JSON_VALUE_NULL)
        bro_json_value_set (out, patch->keys->pdata[i], real_merge_patch (NULL, p));
    }
  return out;
}

static BroJsonValue *
real_fill (RealRun *run, BroJsonValue *v)
{
  BroJsonValue *c;
  switch (v->kind)
    {
    case BRO_JSON_VALUE_STRING:
      {
        g_autoptr (GString) p = g_string_new (v->string);
        g_string_replace (p, "{root}", run->root, 0);
        return bro_json_value_new_string (p->str);
      }
    case BRO_JSON_VALUE_ARRAY:
      c = bro_json_value_new_array ();
      for (guint i = 0; i < v->items->len; i++)
        bro_json_value_append (c, real_fill (run, v->items->pdata[i]));
      return c;
    case BRO_JSON_VALUE_OBJECT:
      c = bro_json_value_new_object ();
      for (guint i = 0; i < v->keys->len; i++)
        bro_json_value_set (c, v->keys->pdata[i], real_fill (run, v->items->pdata[i]));
      return c;
    default:
      return bro_json_value_ref (v);
    }
}

/* config3-scenario-base.json with the scenario's profile over the default one, {root} filled in. */
static BroJsonValue *
real_configuration (RealRun *run)
{
  g_autoptr (BroJsonValue) config = bro_test_fixture_json ("config/config3-scenario-base.json");
  BroJsonValue *given = J (run->scenario, "given");
  BroJsonValue *profile = given ? J (given, "profile") : NULL;
  if (profile)
    {
      BroJsonValue *old = J (config, "profiles");
      BroJsonValue *profiles = bro_json_value_new_array ();
      g_autoptr (BroJsonValue) patch = bro_json_value_new_object ();
      BroJsonValue *merged;
      for (guint i = 0; i < bro_json_value_length (old); i++)
        {
          BroJsonValue *one = bro_json_value_at (old, i);
          bro_json_value_append (profiles, g_strcmp0 (S (one, "id", ""), "default") == 0 ? real_merge_patch (one, profile) : bro_json_value_ref (one));
        }
      bro_json_value_set (patch, "profiles", profiles);
      merged = real_merge_patch (config, patch);
      bro_json_value_unref (config);
      config = merged;
    }
  return real_fill (run, config);
}

static gboolean
real_write (const char *path, GBytes *bytes)
{
  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);
  return g_file_set_contents (path, data, size, NULL);
}

static gboolean
real_prepare (RealRun *run)
{
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  g_autoptr (BroLibraryMarker) marker = bro_library_marker_make ("lib", run->library, "Library", bro_clock_now (BRO_CLOCK (clock)));
  g_autoptr (GBytes) marker_bytes = bro_library_marker_encode (marker);
  g_autoptr (BroJsonValue) config = real_configuration (run);
  g_autoptr (GBytes) config_bytes = bro_json_value_encode_canonical (config);
  g_autofree char *marker_path = g_build_filename (run->library, ".bromelia-library", NULL);
  g_autofree char *config_path = g_build_filename (run->data, "config.json", NULL);
  g_mkdir_with_parents (run->data, 0700);
  g_mkdir_with_parents (run->home, 0700);
  g_mkdir_with_parents (run->library, 0700);
  return real_write (marker_path, marker_bytes) && real_write (config_path, config_bytes);
}

/* ---- the engine's answers ---- */

static void
real_job_done (const BroJobDto *job, const BroBroError *error, gpointer data)
{
  Answer *a = data;
  if (job)
    a->json = bro_job_dto_json (job);
  answer_finish (a, error);
}

static void
real_detail_done (const BroJobDetailDto *detail, const BroBroError *error, gpointer data)
{
  Answer *a = data;
  if (detail)
    a->json = bro_job_detail_dto_json (detail);
  answer_finish (a, error);
}

static void
real_page_done (const BroJobPageDto *page, const BroBroError *error, gpointer data)
{
  Answer *a = data;
  if (page)
    a->json = bro_job_page_dto_json (page);
  answer_finish (a, error);
}

static void
real_log_done (GPtrArray *items, const BroBroError *error, gpointer data)
{
  Answer *a = data;
  if (items)
    {
      a->json = bro_json_value_new_array ();
      for (guint i = 0; i < items->len; i++)
        bro_json_value_append (a->json, bro_log_line_dto_json (g_ptr_array_index (items, i)));
    }
  answer_finish (a, error);
}

/* The answer's JSON (a reference), or NULL with the failure. */
static BroJsonValue *
real_answer (RealRun *run, Answer *a)
{
  BroJsonValue *json = NULL;
  if (!answer_wait (a, 60))
    real_fail (run, "the call to end", "it never ended within 60 s");
  else if (a->error)
    real_failf (run, "the call to succeed", "error %s", a->error);
  else
    json = a->json ? bro_json_value_ref (a->json) : bro_json_value_new_object ();
  answer_clear (a);
  return json;
}

static BroJsonValue *
real_job (RealRun *run, const char *id)
{
  Answer a = { 0 };
  BroId job = { 0 };
  g_strlcpy (job.value, id, sizeof job.value);
  bro_engine_api_job (BRO_ENGINE_API (run->engine), &job, real_detail_done, &a, NULL);
  return real_answer (run, &a);
}

static BroJsonValue *
real_log (RealRun *run, const char *id)
{
  Answer a = { 0 };
  BroId job = { 0 };
  g_strlcpy (job.value, id, sizeof job.value);
  bro_engine_api_job_log (BRO_ENGINE_API (run->engine), &job, 0, real_log_done, &a, NULL);
  return real_answer (run, &a);
}

/* ---- the steps ---- */

static gboolean
real_rip (RealRun *run, BroJsonValue *v)
{
  g_autofree char *base = g_path_get_basename (run->iso);
  char *dot = strrchr (base, '.');
  g_autoptr (BroJobRequest) request = NULL;
  g_autoptr (BroDiscRequest) details = NULL;
  g_autoptr (BroJsonValue) job = NULL;
  Answer a = { 0 };
  if (dot)
    *dot = '\0';
  request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, base);
  request->options = bro_json_value_new_object ();
  details = bro_disc_request_new (bro_disc_source_new (BRO_DISC_SOURCE_ISO, run->iso), NULL);
  request->details = bro_disc_request_encode (details);
  bro_local_engine_start_job (run->engine, request, real_job_done, &a, NULL);
  if (!(job = real_answer (run, &a)))
    return FALSE;
  g_ptr_array_add (run->names, S (v, "as", NULL) ? g_strdup (S (v, "as", "")) : g_strdup_printf ("job%u", run->names->len + 1));
  g_ptr_array_add (run->ids, g_strdup (S (job, "id", "")));
  return TRUE;
}

static char *
real_steps (const BroJsonValue *detail)
{
  BroJsonValue *steps = J (detail, "steps");
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < bro_json_value_length (steps); i++)
    {
      BroJsonValue *s = bro_json_value_at (steps, i);
      g_string_append_printf (out, "%s%s:%s", i ? " " : "", S (s, "kind", "?"), S (s, "state", "?"));
    }
  return g_string_free (out, FALSE);
}

/* Real time (decision 302): until every job has finished, or the cap, which fails with the state and the step. */
static gboolean
real_settle (RealRun *run)
{
  int minutes = g_strcmp0 (run->id, "realdisc-one-pass") == 0 ? 60 : 10;
  gint64 deadline = g_get_monotonic_time () + (gint64) minutes * 60 * G_USEC_PER_SEC;
  for (guint i = 0; i < run->ids->len; i++)
    while (TRUE)
      {
        g_autoptr (BroJsonValue) detail = real_job (run, run->ids->pdata[i]);
        if (!detail)
          return FALSE;
        if (g_strcmp0 (S (detail, "state", ""), "finished") == 0)
          break;
        if (g_get_monotonic_time () > deadline)
          {
            g_autofree char *expected = g_strdup_printf ("job %s to finish within %d min", (char *) run->names->pdata[i], minutes);
            g_autofree char *steps = real_steps (detail);
            return real_failf (run, expected, "%s, steps %s", S (detail, "state", ""), steps);
          }
        g_usleep (G_USEC_PER_SEC);
      }
  return TRUE;
}

/* The job called @name, or the only one for NULL: its index; -1 with the failure. */
static int
real_named (RealRun *run, const char *name)
{
  if (!name)
    {
      if (run->ids->len == 1)
        return 0;
      real_failf (run, "one job, or a name", "%u jobs", run->ids->len);
      return -1;
    }
  for (guint i = 0; i < run->names->len; i++)
    if (g_str_equal (run->names->pdata[i], name))
      return (int) i;
  {
    g_autofree char *expected = g_strconcat ("a job named ", name, NULL);
    real_fail (run, expected, "none");
  }
  return -1;
}

/* Objects by every wanted member, arrays element by element, numbers by value. */
static gboolean
real_matches (const BroJsonValue *want, const BroJsonValue *got)
{
  switch (want->kind)
    {
    case BRO_JSON_VALUE_NULL:
      return !got || got->kind == BRO_JSON_VALUE_NULL;
    case BRO_JSON_VALUE_OBJECT:
      if (!got || got->kind != BRO_JSON_VALUE_OBJECT)
        return FALSE;
      for (guint i = 0; i < want->keys->len; i++)
        if (!real_matches (want->items->pdata[i], J (got, want->keys->pdata[i])))
          return FALSE;
      return TRUE;
    case BRO_JSON_VALUE_ARRAY:
      if (!got || got->kind != BRO_JSON_VALUE_ARRAY || got->items->len != want->items->len)
        return FALSE;
      for (guint i = 0; i < want->items->len; i++)
        if (!real_matches (want->items->pdata[i], got->items->pdata[i]))
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
real_same (RealRun *run, const BroJsonValue *want, const BroJsonValue *got, const char *what)
{
  g_autofree char *w = NULL, *g = NULL, *expected = NULL;
  if (real_matches (want, got))
    return TRUE;
  w = real_text (want);
  g = real_text (got);
  expected = g_strdup_printf ("%s %s", what, w);
  return real_fail (run, expected, g);
}

static gboolean
real_expect_job (RealRun *run, BroJsonValue *want)
{
  int at = real_named (run, S (want, "name", NULL));
  const char *name;
  g_autoptr (BroJsonValue) detail = NULL;
  gboolean ok = TRUE;
  if (at < 0)
    return FALSE;
  name = run->names->pdata[at];
  if (!(detail = real_job (run, run->ids->pdata[at])))
    return FALSE;
  for (guint i = 0; ok && i < want->keys->len; i++)
    {
      const char *key = want->keys->pdata[i];
      BroJsonValue *value = want->items->pdata[i];
      g_autofree char *what = g_strdup_printf ("job %s %s", name, key);
      if (g_str_equal (key, "state") || g_str_equal (key, "outcome") || g_str_equal (key, "error"))
        ok = real_same (run, value, J (detail, key), what);
      else if (g_str_equal (key, "noError"))
        {
          if (bro_json_value_get_bool (value, FALSE) && J (detail, "error"))
            {
              g_autofree char *text = real_text (J (detail, "error"));
              g_autofree char *expected = g_strdup_printf ("job %s without an error", name);
              ok = real_fail (run, expected, text);
            }
        }
      else if (g_str_equal (key, "files"))
        {
          g_autoptr (BroJsonValue) count = bro_json_value_new_integer (bro_json_value_length (J (detail, "files")));
          ok = real_same (run, value, count, what);
        }
    }
  if (!ok)
    {
      g_autofree char *error = real_text (J (detail, "error"));
      g_autofree char *steps = real_steps (detail);
      g_autoptr (BroJsonValue) log = real_log (run, run->ids->pdata[at]);
      GString *more = g_string_new (run->failure);
      g_string_append_printf (more, "\n  (job %s: %s, error %s, steps %s, log:", name, S (detail, "state", ""), error, steps);
      for (guint i = 0; i < bro_json_value_length (log); i++)
        {
          BroJsonValue *l = bro_json_value_at (log, i);
          g_autofree char *params = real_text (J (l, "params"));
          g_string_append_printf (more, "\n    %s %s", S (l, "code", "?"), params);
        }
      g_string_append (more, ")");
      g_free (run->failure);
      run->failure = g_string_free (more, FALSE);
    }
  return ok;
}

static gboolean
real_expect_jobs (RealRun *run, BroJsonValue *want)
{
  g_autoptr (BroJobQueryDto) query = bro_job_query_dto_new ();
  g_autoptr (BroJsonValue) page = NULL;
  static const struct {
    const char *want, *field;
  } lists[] = { { "outcomes", "outcome" }, { "states", "state" }, { "kinds", "kind" } };
  BroJsonValue *items;
  Answer a = { 0 };
  query->limit = BRO_JOB_QUERY_DTO_MAX_LIMIT;
  bro_engine_api_jobs (BRO_ENGINE_API (run->engine), query, real_page_done, &a, NULL);
  if (!(page = real_answer (run, &a)))
    return FALSE;
  items = J (page, "items");
  if (J (want, "count"))
    {
      g_autoptr (BroJsonValue) count = bro_json_value_new_integer (bro_json_value_length (items));
      if (!real_same (run, J (want, "count"), count, "jobs count"))
        return FALSE;
    }
  for (guint k = 0; k < G_N_ELEMENTS (lists); k++)
    if (J (want, lists[k].want))
      {
        g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
        g_autofree char *what = g_strconcat ("jobs ", lists[k].want, NULL);
        for (guint i = 0; i < bro_json_value_length (items); i++)
          {
            BroJsonValue *field = J (bro_json_value_at (items, i), lists[k].field);
            bro_json_value_append (got, field ? bro_json_value_ref (field) : bro_json_value_new_null ());
          }
        if (!real_same (run, J (want, lists[k].want), got, what))
          return FALSE;
      }
  return TRUE;
}

static gboolean
real_log_has (const BroJsonValue *log, const char *code)
{
  for (guint i = 0; i < bro_json_value_length (log); i++)
    if (g_strcmp0 (S (bro_json_value_at (log, i), "code", ""), code) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
real_expect_log (RealRun *run, BroJsonValue *want, const char *named)
{
  int at = real_named (run, S (want, "job", named));
  g_autoptr (BroJsonValue) log = NULL;
  BroJsonValue *include = J (want, "include"), *exclude = J (want, "exclude");
  if (at < 0 || !(log = real_log (run, run->ids->pdata[at])))
    return FALSE;
  for (guint i = 0; i < bro_json_value_length (include); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (include, i), "");
      if (!real_log_has (log, c))
        {
          g_autofree char *expected = g_strdup_printf ("%s in the log of %s", c, (char *) run->names->pdata[at]);
          g_autofree char *text = real_text (log);
          return real_fail (run, expected, text);
        }
    }
  for (guint i = 0; i < bro_json_value_length (exclude); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (exclude, i), "");
      if (real_log_has (log, c))
        {
          g_autofree char *expected = g_strdup_printf ("no %s in the log of %s", c, (char *) run->names->pdata[at]);
          g_autofree char *text = real_text (log);
          return real_fail (run, expected, text);
        }
    }
  return TRUE;
}

/* The makemkvcon runs of the job (decision 304): each run's "==== … $ <command>" header in its makemkv.txt (char *). */
static GPtrArray *
real_calls (RealRun *run, const char *id)
{
  GPtrArray *calls = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *path = g_build_filename (run->data, "jobs", id, BRO_SEAL_STEP_TRANSCRIPT, NULL);
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return calls;
  lines = g_strsplit (text, "\n", -1);
  for (char **l = lines; *l; l++)
    {
      const char *at = g_str_has_prefix (*l, "==== ") ? strstr (*l, " $ ") : NULL;
      if (at)
        g_ptr_array_add (calls, g_strdup (at + 3));
    }
  return calls;
}

static guint
real_calls_with (GPtrArray *calls, const char *part)
{
  guint n = 0;
  for (guint i = 0; i < calls->len; i++)
    if (strstr (calls->pdata[i], part))
      n++;
  return n;
}

static char *
real_show_calls (GPtrArray *calls)
{
  GString *out = g_string_new (NULL);
  g_string_append_printf (out, "%u calls:", calls->len);
  for (guint i = 0; i < calls->len; i++)
    g_string_append_printf (out, "\n  %s", (char *) calls->pdata[i]);
  return g_string_free (out, FALSE);
}

static gboolean
real_expect_calls (RealRun *run, BroJsonValue *w, const char *named)
{
  int at = real_named (run, named);
  g_autoptr (GPtrArray) calls = NULL;
  g_autofree char *shown = NULL;
  BroJsonValue *include = J (w, "include"), *exclude = J (w, "exclude"), *counts = J (w, "counts");
  if (at < 0)
    return FALSE;
  calls = real_calls (run, run->ids->pdata[at]);
  shown = real_show_calls (calls);
  if (bro_json_value_get_bool (J (w, "none"), FALSE) && calls->len > 0)
    return real_fail (run, "no makemkvcon call", shown);
  if (J (w, "count") && bro_json_value_get_integer (J (w, "count"), -1) != calls->len)
    {
      g_autofree char *expected = g_strdup_printf ("%" G_GINT64_FORMAT " makemkvcon calls", bro_json_value_get_integer (J (w, "count"), -1));
      return real_fail (run, expected, shown);
    }
  for (guint i = 0; i < bro_json_value_length (include); i++)
    {
      const char *s = bro_json_value_get_string (bro_json_value_at (include, i), "");
      if (real_calls_with (calls, s) == 0)
        {
          g_autofree char *expected = g_strdup_printf ("a makemkvcon call with \"%s\"", s);
          return real_fail (run, expected, shown);
        }
    }
  for (guint i = 0; i < bro_json_value_length (exclude); i++)
    {
      const char *s = bro_json_value_get_string (bro_json_value_at (exclude, i), "");
      if (real_calls_with (calls, s) > 0)
        {
          g_autofree char *expected = g_strdup_printf ("no makemkvcon call with \"%s\"", s);
          return real_fail (run, expected, shown);
        }
    }
  for (guint i = 0; counts && i < counts->keys->len; i++)
    {
      gint64 n = bro_json_value_get_integer (counts->items->pdata[i], -1);
      if (real_calls_with (calls, counts->keys->pdata[i]) != n)
        {
          g_autofree char *expected = g_strdup_printf ("%" G_GINT64_FORMAT " makemkvcon calls with \"%s\"", n, (char *) counts->keys->pdata[i]);
          return real_fail (run, expected, shown);
        }
    }
  return TRUE;
}

/* Every file and folder under @folder, relative to it, folders without a final '/'. */
static void
real_walk (const char *folder, const char *rel, GPtrArray *files, GPtrArray *folders)
{
  g_autoptr (GDir) dir = g_dir_open (folder, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree char *path = g_build_filename (folder, name, NULL);
      g_autofree char *r = rel[0] ? g_strconcat (rel, "/", name, NULL) : g_strdup (name);
      if (g_file_test (path, G_FILE_TEST_IS_DIR))
        {
          g_ptr_array_add (folders, g_strdup (r));
          real_walk (path, r, files, folders);
        }
      else
        g_ptr_array_add (files, g_strdup (r));
    }
}

static gboolean
real_hidden (const char *path)
{
  return path[0] == '.' || strstr (path, "/.") != NULL;
}

static char *
real_show (GPtrArray *items)
{
  GString *out = g_string_new ("[");
  for (guint i = 0; i < items->len; i++)
    g_string_append_printf (out, "%s\"%s\"", i ? ", " : "", (char *) items->pdata[i]);
  g_string_append (out, "]");
  return g_string_free (out, FALSE);
}

static gboolean
real_glob_parts (char **pats, char **parts)
{
  if (!*pats)
    return !*parts;
  if (g_str_equal (*pats, "**"))
    {
      for (char **p = parts;; p++)
        {
          if (real_glob_parts (pats + 1, p))
            return TRUE;
          if (!*p)
            return FALSE;
        }
    }
  if (!*parts)
    return FALSE;
  if ((*parts)[0] == '.' && (*pats)[0] != '.')
    return FALSE;
  return g_pattern_match_simple (*pats, *parts) && real_glob_parts (pats + 1, parts + 1);
}

/* A path pattern: * within one component, ** across components; a component starting with '.' is matched only by a pattern
 * component that writes the dot. */
static gboolean
real_glob (const char *pattern, const char *path)
{
  g_autofree char *p = g_strdup (pattern), *t = g_strdup (path);
  g_auto (GStrv) pats = NULL, parts = NULL;
  if (g_str_has_suffix (p, "/"))
    p[strlen (p) - 1] = '\0';
  if (g_str_has_suffix (t, "/"))
    t[strlen (t) - 1] = '\0';
  pats = g_strsplit (p, "/", -1);
  parts = g_strsplit (t, "/", -1);
  return real_glob_parts (pats, parts) && (!g_str_has_suffix (pattern, "/") || g_str_has_suffix (path, "/"));
}

static gboolean
real_any_glob (GPtrArray *items, const char *pattern)
{
  for (guint i = 0; i < items->len; i++)
    if (real_glob (pattern, items->pdata[i]))
      return TRUE;
  return FALSE;
}

static gint
real_sort (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(char *const *) a, *(char *const *) b);
}

static gboolean
real_expect_files (RealRun *run, BroJsonValue *want)
{
  const char *under = S (want, "under", NULL);
  g_autofree char *folder = g_build_filename (run->root, under ? under : "", NULL);
  g_autofree char *place = under ? g_strconcat (" under ", under, NULL) : g_strdup ("");
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) folders = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) all = g_ptr_array_new ();
  BroJsonValue *exactly = J (want, "exactly"), *include = J (want, "include"), *exclude = J (want, "exclude");
  real_walk (folder, "", files, folders);
  g_ptr_array_extend (all, files, NULL, NULL);
  g_ptr_array_extend (all, folders, NULL, NULL);
  if (exactly)
    {
      g_autoptr (GPtrArray) listed = g_ptr_array_new_with_free_func (g_free);
      gboolean ok = TRUE;
      for (guint i = 0; i < files->len; i++)
        if (!real_hidden (files->pdata[i]))
          g_ptr_array_add (listed, g_strdup (files->pdata[i]));
      for (guint i = 0; i < folders->len; i++)
        {
          g_autofree char *prefix = g_strconcat (folders->pdata[i], "/", NULL);
          gboolean empty = TRUE;
          for (guint k = 0; empty && k < all->len; k++)
            empty = !g_str_has_prefix (all->pdata[k], prefix);
          if (empty && !real_hidden (folders->pdata[i]))
            g_ptr_array_add (listed, g_steal_pointer (&prefix));
        }
      g_ptr_array_sort (listed, real_sort);
      for (guint i = 0; i < bro_json_value_length (exactly); i++)
        ok = ok && real_any_glob (listed, bro_json_value_get_string (bro_json_value_at (exactly, i), ""));
      for (guint i = 0; ok && i < listed->len; i++)
        {
          gboolean any = FALSE;
          for (guint k = 0; !any && k < bro_json_value_length (exactly); k++)
            any = real_glob (bro_json_value_get_string (bro_json_value_at (exactly, k), ""), listed->pdata[i]);
          ok = any;
        }
      if (!ok)
        {
          g_autofree char *text = real_text (exactly);
          g_autofree char *expected = g_strdup_printf ("exactly %s%s", text, place);
          g_autofree char *found = real_show (listed);
          return real_fail (run, expected, found);
        }
    }
  for (guint i = 0; i < bro_json_value_length (include); i++)
    {
      const char *p = bro_json_value_get_string (bro_json_value_at (include, i), "");
      if (!real_any_glob (all, p))
        {
          g_autofree char *expected = g_strconcat (p, place, NULL);
          g_autofree char *found = real_show (all);
          return real_fail (run, expected, found);
        }
    }
  for (guint i = 0; i < bro_json_value_length (exclude); i++)
    {
      const char *p = bro_json_value_get_string (bro_json_value_at (exclude, i), "");
      if (real_any_glob (all, p))
        {
          g_autofree char *expected = g_strconcat ("no ", p, place, NULL);
          g_autofree char *found = real_show (all);
          return real_fail (run, expected, found);
        }
    }
  return TRUE;
}

/* What's wrong with the folder's SHA256SUMS (char *), every file it lists hashed again from disk; what it lists in @listed. */
static GPtrArray *
real_check_sums (const char *folder, GPtrArray **listed)
{
  GPtrArray *bad = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *sums = g_build_filename (folder, "SHA256SUMS", NULL);
  g_autofree char *text = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  g_file_get_contents (sums, &text, NULL, NULL);
  entries = bro_sha256_sums_parse (text ? text : "");
  if (listed)
    *listed = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < entries->len; i++)
    {
      BroSumEntry *e = entries->pdata[i];
      g_autofree char *path = g_build_filename (folder, e->path, NULL);
      FILE *file = fopen (path, "rb");
      if (listed)
        g_ptr_array_add (*listed, g_strdup (e->path));
      if (!file)
        g_ptr_array_add (bad, g_strconcat (e->path, " missing", NULL));
      else
        {
          g_autoptr (BroSha256Hasher) hasher = bro_sha256_hasher_new ();
          g_autofree guint8 *chunk = g_malloc (1 << 22);
          g_autofree char *sha = NULL;
          size_t n;
          while ((n = fread (chunk, 1, 1 << 22, file)) > 0)
            bro_sha256_hasher_update (hasher, chunk, n);
          fclose (file);
          sha = bro_sha256_hasher_finish (hasher);
          if (g_strcmp0 (sha, e->sha256) != 0)
            g_ptr_array_add (bad, g_strconcat (e->path, " differs", NULL));
        }
    }
  return bad;
}

static gboolean
real_expect_sums (RealRun *run, BroJsonValue *want)
{
  const char *rel = S (want, "folder", "");
  g_autofree char *folder = g_build_filename (run->root, rel, NULL);
  g_autofree char *sums = g_build_filename (folder, "SHA256SUMS", NULL);
  g_autoptr (GPtrArray) bad = NULL;
  g_autoptr (GPtrArray) listed = NULL;
  BroJsonValue *ok = J (want, "ok"), *files = J (want, "files");
  if (!g_file_test (sums, G_FILE_TEST_EXISTS))
    {
      g_autofree char *expected = g_strconcat (rel, "/SHA256SUMS", NULL);
      return real_fail (run, expected, "none");
    }
  bad = real_check_sums (folder, &listed);
  if (ok && (bad->len == 0) != bro_json_value_get_bool (ok, FALSE))
    {
      g_autofree char *expected = g_strdup_printf ("sha256sum -c in %s %s", rel, bro_json_value_get_bool (ok, FALSE) ? "passing" : "failing");
      g_autofree char *found = bad->len == 0 ? g_strdup ("it passes") : real_show (bad);
      return real_fail (run, expected, found);
    }
  if (files)
    {
      g_autoptr (GPtrArray) wanted = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *w = NULL, *g = NULL;
      for (guint i = 0; i < bro_json_value_length (files); i++)
        g_ptr_array_add (wanted, g_strdup (bro_json_value_get_string (bro_json_value_at (files, i), "")));
      g_ptr_array_sort (wanted, real_sort);
      g_ptr_array_sort (listed, real_sort);
      w = real_show (wanted);
      g = real_show (listed);
      if (g_strcmp0 (w, g) != 0)
        {
          g_autofree char *expected = g_strdup_printf ("the files %s/SHA256SUMS lists %s", rel, w);
          return real_fail (run, expected, g);
        }
    }
  return TRUE;
}

static gboolean
real_expect (RealRun *run, BroJsonValue *expect)
{
  BroJsonValue *job = J (expect, "job");
  const char *named = job ? S (job, "name", NULL) : NULL;
  if (!real_settle (run))
    return FALSE;
  for (guint i = 0; i < expect->keys->len; i++)
    {
      const char *key = expect->keys->pdata[i];
      BroJsonValue *want = expect->items->pdata[i];
      gboolean ok = TRUE;
      if (g_str_equal (key, "job"))
        ok = real_expect_job (run, want);
      else if (g_str_equal (key, "jobs"))
        ok = real_expect_jobs (run, want);
      else if (g_str_equal (key, "log"))
        ok = real_expect_log (run, want, named);
      else if (g_str_equal (key, "calls"))
        ok = real_expect_calls (run, J (want, "makemkvcon"), named);
      else if (g_str_equal (key, "files"))
        ok = real_expect_files (run, want);
      else if (g_str_equal (key, "sums"))
        ok = real_expect_sums (run, want);
      if (!ok)
        return FALSE;
    }
  return TRUE;
}

/* ---- the end ---- */

/* What every real run must leave true, whatever its scenario says: no makemkvcon run named a drive, nothing was written to
 * the empty home, every SHA256SUMS in the library holds from disk. Then the run's line for the report. */
static gboolean
real_finish (RealRun *run)
{
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) folders = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) home = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) home_folders = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) shown = g_ptr_array_new ();
  g_autofree char *list = NULL;
  gint64 bytes = 0;
  guint sums = 0;
  Answer a = { 0 };
  run->step++;
  run->kind = "end";
  for (guint i = 0; i < run->ids->len; i++)
    {
      g_autoptr (GPtrArray) calls = real_calls (run, run->ids->pdata[i]);
      if (real_calls_with (calls, " disc:") > 0 || real_calls_with (calls, " dev:") > 0)
        {
          g_autofree char *expected = g_strdup_printf ("job %s reading only the image", (char *) run->names->pdata[i]);
          g_autofree char *shown_calls = real_show_calls (calls);
          return real_fail (run, expected, shown_calls);
        }
    }
  real_walk (run->home, "", home, home_folders);
  if (home->len + home_folders->len > 0)
    {
      g_autofree char *found = real_show (home->len ? home : home_folders);
      return real_fail (run, "the home left empty", found);
    }
  bro_engine_api_drives (BRO_ENGINE_API (run->engine), answer_list, &a, NULL);
  if (!answer_wait (&a, 60) || a.error || a.count != 0)
    {
      answer_clear (&a);
      return real_fail (run, "no drives", "some");
    }
  answer_clear (&a);
  real_walk (run->library, "", files, folders);
  g_ptr_array_sort (files, real_sort);
  for (guint i = 0; i < files->len; i++)
    {
      g_autofree char *path = g_build_filename (run->library, files->pdata[i], NULL);
      GStatBuf st;
      if (g_stat (path, &st) == 0)
        bytes += st.st_size;
      if (!real_hidden (files->pdata[i]))
        g_ptr_array_add (shown, files->pdata[i]);
      if (g_str_has_suffix (files->pdata[i], "/SHA256SUMS"))
        {
          g_autofree char *folder = g_path_get_dirname (path);
          g_autoptr (GPtrArray) bad = real_check_sums (folder, NULL);
          if (bad->len > 0)
            {
              g_autofree char *expected = g_strconcat ("sha256sum -c passing in ", files->pdata[i], NULL);
              g_autofree char *found = real_show (bad);
              return real_fail (run, expected, found);
            }
          sums++;
        }
    }
  list = real_show (shown);
  g_printerr ("%s: %" G_GINT64_FORMAT " s, %u files in the library (%" G_GINT64_FORMAT " bytes), %u SHA256SUMS hold: %s\n", run->id,
              (g_get_monotonic_time () - run->started) / G_USEC_PER_SEC, shown->len, bytes, sums, list);
  return TRUE;
}

static void
real_copy (const char *from, const char *to)
{
  g_autofree char *text = NULL;
  gsize length;
  if (g_file_get_contents (from, &text, &length, NULL))
    g_file_set_contents (to, text, length, NULL);
}

/* The engine log and each job folder's own files (its logs and transcripts; never its home), to BROMELIA_TEST_OUTPUT or the
 * temporary folder. */
static void
real_keep_logs (RealRun *run)
{
  const char *base = g_getenv ("BROMELIA_TEST_OUTPUT");
  g_autofree char *output = g_build_filename (base && base[0] ? base : g_get_tmp_dir (), "bromelia-realdisc-failed", run->id, NULL);
  g_autofree char *engine_log = g_build_filename (run->data, "logs", "engine.log", NULL);
  g_autofree char *kept = g_build_filename (output, "engine.log", NULL);
  g_mkdir_with_parents (output, 0700);
  real_copy (engine_log, kept);
  for (guint i = 0; i < run->ids->len; i++)
    {
      g_autofree char *folder = g_build_filename (run->data, "jobs", run->ids->pdata[i], NULL);
      g_autoptr (GDir) dir = g_dir_open (folder, 0, NULL);
      const char *name;
      while (dir && (name = g_dir_read_name (dir)))
        {
          g_autofree char *from = g_build_filename (folder, name, NULL);
          g_autofree char *to_name = g_strconcat (run->ids->pdata[i], "-", name, NULL);
          g_autofree char *to = g_build_filename (output, to_name, NULL);
          if (g_file_test (from, G_FILE_TEST_IS_REGULAR))
            real_copy (from, to);
        }
    }
  g_printerr ("%s: logs kept in %s\n", run->id, output);
}

static void
real_stop (RealRun *run)
{
  Answer a = { 0 };
  if (!run->engine)
    return;
  bro_engine_api_stop (BRO_ENGINE_API (run->engine), answer_done, &a, NULL);
  if (!answer_wait (&a, 60) || a.error)
    g_printerr ("%s: stopping the engine: %s\n", run->id, a.error ? a.error : "it never ended");
  answer_clear (&a);
  g_clear_object (&run->engine);
}

static gboolean
real_play (RealRun *run)
{
  g_autoptr (GHashTable) environment = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (BroBroError) error = NULL;
  BroRootOptions options = { run->data, NULL, run->home, FALSE, TRUE, FALSE, environment };
  BroJsonValue *steps = J (run->scenario, "steps");
  Answer a = { 0 };
  g_hash_table_insert (environment, "PATH", (gpointer) (g_getenv ("PATH") ? g_getenv ("PATH") : ""));
  if (!real_prepare (run))
    return real_fail (run, "the world to be written", "it wasn't");
  if (!(run->engine = bro_composition_root_build (&options, &error)))
    return real_failf (run, "the real root to build", "error %s", error->code);
  bro_local_engine_start (run->engine, answer_done, &a, NULL);
  if (!answer_wait (&a, 60) || a.error)
    {
      real_failf (run, "the engine to start", "%s", a.error ? a.error : "it never started");
      answer_clear (&a);
      return FALSE;
    }
  answer_clear (&a);
  for (guint i = 0; i < bro_json_value_length (steps); i++)
    {
      BroJsonValue *step = bro_json_value_at (steps, i);
      run->step = (int) i + 1;
      run->kind = step->keys->pdata[0];
      if (!(g_str_equal (run->kind, "rip") ? real_rip (run, step->items->pdata[0]) : real_expect (run, step->items->pdata[0])))
        return FALSE;
    }
  return real_finish (run);
}

/* Why it doesn't run here, with the room it would have (realdisc-one-pass writes 13.8 GB). */
static char *
real_skip_reason (const char *id, const char *variable)
{
  struct statvfs room = { 0 };
  double free_gb;
  statvfs (g_get_tmp_dir (), &room);
  free_gb = (double) room.f_bavail * (double) room.f_frsize / 1e9;
  return g_strdup_printf ("%s: %s isn't set (%.1f GB free in %s)", id, variable, free_gb, g_get_tmp_dir ());
}

static void
test_real_disc (gconstpointer data)
{
  const char *id = data;
  g_autoptr (BroJsonValue) scenario = real_load (id);
  const char *variable = S (J (scenario, "realDisc"), "env", "");
  const char *iso = g_getenv (variable);
  g_autoptr (GError) gerror = NULL;
  g_autofree char *root = NULL;
  RealRun run = { 0 };
#ifdef __APPLE__
  /* Decision 305: the root's Linux layout (HOME/.MakeMKV) doesn't reach macOS makemkvcon: C's real runs are in Docker. */
  g_test_skip ("the C root on macOS: Bromelia's settings wouldn't reach makemkvcon (claudeDocs/tools/rebuild-phase3/realdisc-c.sh runs these in Docker)");
  return;
#endif
  if (!iso || !iso[0])
    {
      g_autofree char *reason = real_skip_reason (id, variable);
      g_printerr ("skipped: %s\n", reason);
      g_test_skip (reason);
      return;
    }
  root = g_dir_make_tmp ("bromelia-realdisc-XXXXXX", &gerror);
  g_assert_no_error (gerror);
  run.id = id;
  run.scenario = scenario;
  run.iso = iso;
  run.root = root;
  run.data = g_build_filename (root, "data", NULL);
  run.home = g_build_filename (root, "home", NULL);
  run.library = g_build_filename (root, "library", NULL);
  run.names = g_ptr_array_new_with_free_func (g_free);
  run.ids = g_ptr_array_new_with_free_func (g_free);
  run.kind = "start";
  run.started = g_get_monotonic_time ();
  if (real_check_supported (&run) && !real_play (&run))
    real_keep_logs (&run);
  real_stop (&run);
  bro_test_remove_tree (root);
  g_free (run.data);
  g_free (run.home);
  g_free (run.library);
  g_ptr_array_unref (run.names);
  g_ptr_array_unref (run.ids);
  if (run.failure)
    {
      g_autofree char *failure = run.failure;
      g_printerr ("%s\n", failure);
      g_test_fail ();
    }
}

/* A later realDisc scenario can't pass half-read: the phase 5 ones fail before anything runs. */
static void
test_what_a_phase_3_run_doesnt_know_fails (void)
{
  static const struct {
    const char *id, *failure;
  } cases[] = {
    { "realdisc-custom-tracks-rename-script", "realdisc-custom-tracks-rename-script step 0 (start): not supported in a real world: given.configPatch" },
    { "realdisc-dvd-navigation-from-iso", "realdisc-dvd-navigation-from-iso step 1 (open): not supported in a real world: open" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (BroJsonValue) scenario = real_load (cases[i].id);
      RealRun run = { .id = cases[i].id, .scenario = scenario, .kind = "start" };
      g_assert_false (real_check_supported (&run));
      g_assert_cmpstr (run.failure, ==, cases[i].failure);
      g_free (run.failure);
    }
}

/* The cases' ids, which the cases point into (a case left out by -p never frees what it was given). */
static GPtrArray *
real_disc_add_tests (void)
{
  GPtrArray *ids = real_selected ();
  g_test_add_func ("/hosts/what-a-phase-3-run-doesnt-know-fails", test_what_a_phase_3_run_doesnt_know_fails);
  for (guint i = 0; i < ids->len; i++)
    {
      g_autofree char *path = g_strconcat ("/real-disc/", ids->pdata[i], NULL);
      g_test_add_data_func (path, ids->pdata[i], test_real_disc);
    }
  return ids;
}

#undef S
#undef J
