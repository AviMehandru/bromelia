/* bro-scenario-expect.c */
#include "bro-scenario-expect.h"
#include "bro-test-fixtures.h"

#include "bro-acquire-output.h"
#include "bro-archive-files.h"
#include "bro-directory-entry.h"
#include "bro-english-messages.h"
#include "bro-file-info.h"
#include "bro-message-text-renderer.h"
#include "bro-seal-step.h"
#include "bro-sha256-hasher.h"
#include "bro-sha256-sums.h"
#include "bro-step-record.h"
#include "bro-sum-entry.h"

#include <sqlite3.h>
#include <string.h>

#define J(v, k) bro_json_value_member ((v), (k))
#define S(v, k, d) bro_json_value_get_string (J ((v), (k)), (d))

typedef gboolean (*Keyed) (BroScenarioRun *run, BroJsonValue *want, const char *named);

/* ---- helpers ---- */

static gboolean
same (BroScenarioRun *run, const BroJsonValue *want, const BroJsonValue *got, const char *what)
{
  g_autofree char *w = NULL, *g = NULL, *expected = NULL;
  if (bro_scenario_expect_matches (want, got))
    return TRUE;
  w = bro_scenario_json_text (want);
  g = bro_scenario_json_text (got);
  expected = g_strdup_printf ("%s %s", what, w);
  return bro_scenario_run_fail (run, expected, g);
}

/* A glob over a whole string: '*' any run of characters (any byte, '/' and line breaks too). */
static gboolean
star_match (const char *p, const char *t)
{
  if (!*p)
    return !*t;
  if (*p == '*')
    {
      for (const char *x = t;; x++)
        {
          if (star_match (p + 1, x))
            return TRUE;
          if (!*x)
            return FALSE;
        }
    }
  return *t == *p && star_match (p + 1, t + 1);
}

gboolean
bro_scenario_expect_matches (const BroJsonValue *want, const BroJsonValue *got)
{
  switch (want->kind)
    {
    case BRO_JSON_VALUE_NULL:
      return !got || got->kind == BRO_JSON_VALUE_NULL;
    case BRO_JSON_VALUE_STRING:
      if (strchr (want->string, '*'))
        return got && got->kind == BRO_JSON_VALUE_STRING && star_match (want->string, got->string);
      return got && bro_json_value_equal (want, got);
    case BRO_JSON_VALUE_OBJECT:
      if (!got || got->kind != BRO_JSON_VALUE_OBJECT)
        return FALSE;
      for (guint i = 0; i < want->keys->len; i++)
        if (!bro_scenario_expect_matches (want->items->pdata[i], J (got, want->keys->pdata[i])))
          return FALSE;
      return TRUE;
    case BRO_JSON_VALUE_ARRAY:
      if (!got || got->kind != BRO_JSON_VALUE_ARRAY || got->items->len != want->items->len)
        return FALSE;
      for (guint i = 0; i < want->items->len; i++)
        if (!bro_scenario_expect_matches (want->items->pdata[i], got->items->pdata[i]))
          return FALSE;
      return TRUE;
    case BRO_JSON_VALUE_INTEGER:
    case BRO_JSON_VALUE_NUMBER:
      return got && (got->kind == BRO_JSON_VALUE_INTEGER || got->kind == BRO_JSON_VALUE_NUMBER)
             && bro_json_value_get_number (want, 0) == bro_json_value_get_number (got, 0);
    default:
      return got && bro_json_value_equal (want, got);
    }
}

static char *
show (GPtrArray *items)
{
  GString *s = g_string_new ("[");
  for (guint i = 0; i < items->len; i++)
    g_string_append_printf (s, "%s\"%s\"", i ? ", " : "", (char *) items->pdata[i]);
  g_string_append_c (s, ']');
  return g_string_free (s, FALSE);
}

static int
by_text (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static gboolean
contains_string (GPtrArray *list, const char *s)
{
  for (guint i = 0; i < list->len; i++)
    if (!strcmp (list->pdata[i], s))
      return TRUE;
  return FALSE;
}

static GBytes *
read_file (BroScenarioRun *run, const char *path)
{
  g_autoptr (BroBroError) error = NULL;
  return bro_file_system_read (BRO_FILE_SYSTEM (run->root->fs), path, &error);
}

static char *
text_of (GBytes *bytes)
{
  return bytes ? g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes)) : g_strdup ("");
}

static gboolean
exists (BroScenarioRun *run, const char *path)
{
  return bro_file_system_exists (BRO_FILE_SYSTEM (run->root->fs), path);
}

/* ---- placeholders ---- */

static char *
unit8_of (const BroId *id)
{
  BroId unit = bro_archive_files_unit_id (id);
  return g_strndup (unit.value, 8);
}

static char *
fill_text (BroScenarioRun *run, const char *s, const char *named, gboolean *ok)
{
  GString *out = g_string_new (NULL);
  for (const char *c = s; *c;)
    {
      if (g_str_has_prefix (c, "{root}"))
        {
          g_string_append (out, TEST_ROOT);
          c += 6;
          continue;
        }
      if (g_str_has_prefix (c, "{unit8") || g_str_has_prefix (c, "{jobId") || g_str_has_prefix (c, "{driveId:"))
        {
          const char *close = strchr (c, '}');
          gboolean unit = g_str_has_prefix (c, "{unit8"), drive = g_str_has_prefix (c, "{driveId:");
          const char *after = c + (drive ? 9 : 6);
          if (close && (drive || *after == '}' || *after == ':'))
            {
              g_autofree char *name = drive || *after == ':' ? g_strndup (after + (drive ? 0 : 1), close - after - (drive ? 0 : 1)) : NULL;
              if (drive)
                {
                  g_autofree char *id = bro_scenario_run_drive_id (run, name);
                  if (!id)
                    *ok = FALSE;
                  else
                    g_string_append (out, id);
                }
              else
                {
                  const char *job = name ? name : named ? named : bro_scenario_run_only_job (run);
                  BroId id;
                  if (!job || !bro_scenario_run_job (run, job, &id))
                    *ok = FALSE;
                  else if (unit)
                    {
                      g_autofree char *u = unit8_of (&id);
                      g_string_append (out, u);
                    }
                  else
                    g_string_append (out, id.value);
                }
              c = close + 1;
              continue;
            }
        }
      g_string_append_c (out, *c++);
    }
  return g_string_free (out, FALSE);
}

static BroJsonValue *
fill (BroScenarioRun *run, BroJsonValue *v, const char *named, gboolean *ok)
{
  BroJsonValue *c;
  switch (v->kind)
    {
    case BRO_JSON_VALUE_STRING:
      {
        g_autofree char *t = fill_text (run, v->string, named, ok);
        return bro_json_value_new_string (t);
      }
    case BRO_JSON_VALUE_ARRAY:
      c = bro_json_value_new_array ();
      for (guint i = 0; i < v->items->len; i++)
        bro_json_value_append (c, fill (run, v->items->pdata[i], named, ok));
      return c;
    case BRO_JSON_VALUE_OBJECT:
      c = bro_json_value_new_object ();
      for (guint i = 0; i < v->keys->len; i++)
        bro_json_value_set (c, v->keys->pdata[i], fill (run, v->items->pdata[i], named, ok));
      return c;
    default:
      return bro_json_value_ref (v);
    }
}

BroJsonValue *
bro_scenario_expect_fill (BroScenarioRun *run, BroJsonValue *v, const char *named)
{
  gboolean ok = TRUE;
  BroJsonValue *filled = fill (run, v, named, &ok);
  if (ok)
    return filled;
  bro_json_value_unref (filled);
  return NULL;
}

/* ---- when ---- */

static gboolean
holds (BroScenarioRun *run, BroJsonValue *when, gboolean *held)
{
  const char *type = S (when, "event", NULL), *step, *state, *outcome;
  g_autoptr (BroJsonValue) detail = NULL;
  BroId id;
  *held = FALSE;
  if (type)
    {
      g_autoptr (BroJsonValue) events = bro_scenario_run_events (run);
      for (guint i = 0; i < bro_json_value_length (events) && !*held; i++)
        *held = !strcmp (S (bro_json_value_at (events, i), "type", ""), type);
      return TRUE;
    }
  if (!bro_scenario_run_job (run, S (when, "job", ""), &id) || !(detail = bro_scenario_engine_job (run, &id)))
    return FALSE;
  if ((step = S (when, "step", NULL)))
    {
      gboolean started = FALSE;
      BroJsonValue *steps = J (detail, "steps");
      for (guint i = 0; i < bro_json_value_length (steps) && !started; i++)
        started = !strcmp (S (bro_json_value_at (steps, i), "kind", ""), step) && S (bro_json_value_at (steps, i), "startedAt", NULL);
      if (!started)
        return TRUE;
    }
  if ((state = S (when, "state", NULL)) && strcmp (S (detail, "state", ""), state) != 0)
    return TRUE;
  if ((outcome = S (when, "outcome", NULL)) && g_strcmp0 (S (detail, "outcome", NULL), outcome) != 0)
    return TRUE;
  *held = TRUE;
  return TRUE;
}

gboolean
bro_scenario_expect_when (BroScenarioRun *run, BroJsonValue *when)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) (BRO_SCENARIO_PATIENCE * G_USEC_PER_SEC);
  for (;;)
    {
      gboolean held;
      if (!holds (run, when, &held))
        return FALSE;
      if (held)
        return TRUE;
      if (g_get_monotonic_time () > deadline)
        {
          g_autofree char *text = bro_scenario_json_text (when);
          g_autofree char *expected = g_strconcat ("when ", text, NULL);
          return bro_scenario_run_fail (run, expected, "it never held within 10 s");
        }
      g_usleep (5000);
    }
}

/* ---- db ---- */

/* A table's rows, in rowid order, through a read-only connection of its own (a JSON array; NULL with a failure). */
static BroJsonValue *
db_rows (BroScenarioRun *run, const char *table)
{
  g_autofree char *path = g_build_filename (run->root->folder, "bromelia.db", NULL);
  g_autofree char *sql = NULL;
  sqlite3 *db = NULL;
  sqlite3_stmt *st = NULL;
  BroJsonValue *rows;
  for (const char *c = table; *c; c++)
    if (!((*c >= 'a' && *c <= 'z') || *c == '_'))
      {
        bro_scenario_run_fail (run, "a table name", table);
        return NULL;
      }
  if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
      bro_scenario_run_fail (run, "the database", sqlite3_errmsg (db));
      sqlite3_close (db);
      return NULL;
    }
  sql = g_strdup_printf ("SELECT * FROM %s ORDER BY rowid", table);
  if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK)
    {
      bro_scenario_run_fail (run, sql, sqlite3_errmsg (db));
      sqlite3_close (db);
      return NULL;
    }
  rows = bro_json_value_new_array ();
  while (sqlite3_step (st) == SQLITE_ROW)
    {
      BroJsonValue *row = bro_json_value_new_object ();
      for (int i = 0; i < sqlite3_column_count (st); i++)
        {
          BroJsonValue *v;
          switch (sqlite3_column_type (st, i))
            {
            case SQLITE_INTEGER:
              v = bro_json_value_new_integer (sqlite3_column_int64 (st, i));
              break;
            case SQLITE_FLOAT:
              v = bro_json_value_new_number (sqlite3_column_double (st, i));
              break;
            case SQLITE_BLOB:
              {
                const guint8 *b = sqlite3_column_blob (st, i);
                int n = sqlite3_column_bytes (st, i);
                GString *hex = g_string_new (NULL);
                for (int k = 0; k < n; k++)
                  g_string_append_printf (hex, "%02x", b[k]);
                v = bro_json_value_new_string (hex->str);
                g_string_free (hex, TRUE);
                break;
              }
            case SQLITE_NULL:
              v = bro_json_value_new_null ();
              break;
            default:
              v = bro_json_value_new_string ((const char *) sqlite3_column_text (st, i));
            }
          bro_json_value_set (row, sqlite3_column_name (st, i), v);
        }
      bro_json_value_append (rows, row);
    }
  sqlite3_finalize (st);
  sqlite3_close (db);
  return rows;
}

gboolean
bro_scenario_expect_db (BroScenarioRun *run, BroJsonValue *want)
{
  for (guint i = 0; i < want->keys->len; i++)
    {
      const char *table = want->keys->pdata[i];
      BroJsonValue *w = want->items->pdata[i], *count = J (w, "count"), *wanted = J (w, "rows");
      g_autoptr (BroJsonValue) rows = db_rows (run, table);
      if (!rows)
        return FALSE;
      if (count && count->kind == BRO_JSON_VALUE_INTEGER && bro_json_value_length (rows) != (guint) count->integer)
        {
          g_autofree char *expected = g_strdup_printf ("%" G_GINT64_FORMAT " rows in %s", count->integer, table);
          g_autofree char *text = bro_scenario_json_text (rows);
          return bro_scenario_run_failf (run, expected, "%u: %s", bro_json_value_length (rows), text);
        }
      if (wanted)
        {
          g_autofree char *what = g_strconcat ("the rows of ", table, NULL);
          if (!same (run, wanted, rows, what))
            return FALSE;
        }
    }
  return TRUE;
}

/* ---- job, jobs ---- */

static gboolean
job_checks (BroScenarioRun *run, BroJsonValue *want, const char *name, const BroId *id, BroJsonValue *detail)
{
  g_autofree char *what = g_strconcat ("job ", name, NULL);
  for (guint i = 0; i < want->keys->len; i++)
    {
      const char *key = want->keys->pdata[i];
      BroJsonValue *value = want->items->pdata[i];
      g_autofree char *about = g_strdup_printf ("%s %s", what, key);
      if (!strcmp (key, "name"))
        continue;
      if (!strcmp (key, "state") || !strcmp (key, "outcome") || !strcmp (key, "mode") || !strcmp (key, "kind") || !strcmp (key, "queue")
          || !strcmp (key, "automatic") || !strcmp (key, "error") || !strcmp (key, "blockedBy") || !strcmp (key, "decision"))
        {
          if (!same (run, value, J (detail, key), about))
            return FALSE;
        }
      else if (!strcmp (key, "noError"))
        {
          if (bro_json_value_get_bool (value, FALSE) && J (detail, "error"))
            {
              g_autofree char *expected = g_strconcat (what, " without an error", NULL);
              g_autofree char *text = bro_scenario_json_text (J (detail, "error"));
              return bro_scenario_run_fail (run, expected, text);
            }
        }
      else if (!strcmp (key, "errorText"))
        {
          g_autoptr (BroMessageTextRenderer) english = bro_message_text_renderer_new (bro_english_messages_catalog ());
          g_autofree char *text = J (detail, "error") ? bro_message_text_renderer_render_json (english, J (detail, "error")) : g_strdup ("");
          const char *wanted = bro_json_value_get_string (value, "");
          if (!strstr (text, wanted))
            {
              g_autofree char *expected = g_strdup_printf ("%s error text containing \"%s\"", what, wanted);
              g_autofree char *found = g_strdup_printf ("\"%s\"", text);
              return bro_scenario_run_fail (run, expected, found);
            }
        }
      else if (!strcmp (key, "parent"))
        {
          BroId parent;
          g_autoptr (BroJsonValue) want_id = NULL;
          g_autofree char *label = g_strconcat (what, " parent", NULL);
          if (!bro_scenario_run_job (run, bro_json_value_get_string (value, ""), &parent))
            return FALSE;
          want_id = bro_json_value_new_string (parent.value);
          if (!same (run, want_id, J (detail, "parentId"), label))
            return FALSE;
        }
      else if (!strcmp (key, "steps"))
        {
          g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
          BroJsonValue *steps = J (detail, "steps");
          for (guint k = 0; k < bro_json_value_length (steps); k++)
            if (strcmp (S (bro_json_value_at (steps, k), "state", ""), "pending") != 0)
              bro_json_value_append (got, bro_json_value_new_string (S (bro_json_value_at (steps, k), "kind", "")));
          if (!same (run, value, got, about))
            return FALSE;
        }
      else if (!strcmp (key, "files"))
        {
          g_autoptr (BroJsonValue) got = bro_json_value_new_integer (bro_json_value_length (J (detail, "files")));
          if (!same (run, value, got, about))
            return FALSE;
        }
      else if (!strcmp (key, "titles"))
        {
          g_autoptr (GPtrArray) rows = bro_step_repository_load (bro_store_steps (BRO_STORE (run->root->store)), *id, NULL);
          g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
          const BroStepRecord *acquire = NULL;
          for (guint k = 0; rows && k < rows->len; k++)
            {
              const BroStepRecord *r = rows->pdata[k];
              if (r->kind == BRO_STEP_KIND_ACQUIRE && r->output)
                acquire = r;
            }
          if (acquire)
            {
              g_autoptr (BroAcquireOutput) output = bro_acquire_output_decode (acquire->output->data);
              for (guint k = 0; output && k < output->titles->len; k++)
                bro_json_value_append (got, bro_json_value_new_integer (((BroAcquiredTitle *) output->titles->pdata[k])->index));
            }
          if (!same (run, value, got, about))
            return FALSE;
        }
      else if (!strcmp (key, "unit"))
        {
          BroId unit = bro_archive_files_unit_id (id);
          g_autoptr (BroJsonValue) rows = NULL;
          BroJsonValue *row = NULL;
          if (S (detail, "unitId", NULL))
            g_strlcpy (unit.value, S (detail, "unitId", ""), sizeof unit.value);
          if (!(rows = db_rows (run, "archive_units")))
            return FALSE;
          for (guint k = 0; k < bro_json_value_length (rows) && !row; k++)
            if (!g_strcmp0 (S (bro_json_value_at (rows, k), "id", NULL), unit.value))
              row = bro_json_value_at (rows, k);
          if (value->kind == BRO_JSON_VALUE_OBJECT && value->keys->len == 0)
            continue;
          if (!row)
            {
              g_autofree char *text = bro_scenario_json_text (value);
              g_autofree char *expected = g_strdup_printf ("%s unit %s", what, text);
              g_autofree char *found = g_strconcat ("no archive_units row for ", unit.value, NULL);
              return bro_scenario_run_fail (run, expected, found);
            }
          if (!same (run, value, row, about))
            return FALSE;
        }
      else if (!strcmp (key, "counts"))
        {
          g_autoptr (BroJsonValue) events = bro_scenario_run_events (run);
          g_autofree char *label = g_strconcat (what, " counts (its last jobChanged or jobFinished)", NULL);
          BroJsonValue *got = NULL;
          for (guint k = bro_json_value_length (events); k > 0; k--)
            {
              BroJsonValue *e = bro_json_value_at (events, k - 1), *job = J (J (e, "data"), "job");
              const char *type = S (e, "type", "");
              if ((!strcmp (type, "jobChanged") || !strcmp (type, "jobFinished")) && !g_strcmp0 (S (job, "id", NULL), id->value))
                {
                  got = J (job, "counts");
                  break;
                }
            }
          if (!same (run, value, got, label))
            return FALSE;
        }
      else
        return bro_scenario_run_fail (run, "a known job expectation", key);
    }
  return TRUE;
}

gboolean
bro_scenario_expect_job (BroScenarioRun *run, BroJsonValue *want)
{
  const char *name = S (want, "name", NULL);
  g_autoptr (BroJsonValue) detail = NULL;
  g_autoptr (BroJsonValue) log = NULL;
  g_autofree char *error = NULL;
  GString *more;
  BroId id;
  if (!name && !(name = bro_scenario_run_only_job (run)))
    return FALSE;
  if (!bro_scenario_run_job (run, name, &id) || !(detail = bro_scenario_engine_job (run, &id)))
    return FALSE;
  if (job_checks (run, want, name, &id, detail))
    return TRUE;
  /* The job as it is, after the failure, to tell why. */
  error = bro_scenario_json_text (J (detail, "error"));
  more = g_string_new (NULL);
  g_string_append_printf (more, "%s\n  (job %s: %s, error %s, steps", run->failure, name, S (detail, "state", ""), error);
  for (guint i = 0; i < bro_json_value_length (J (detail, "steps")); i++)
    {
      BroJsonValue *s = bro_json_value_at (J (detail, "steps"), i);
      g_string_append_printf (more, " %s:%s", S (s, "kind", ""), S (s, "state", ""));
    }
  g_string_append (more, ", log:");
  {
    g_autofree char *kept = g_strdup (run->failure);
    log = bro_scenario_engine_job_log (run, &id);
    g_free (run->failure);
    run->failure = g_steal_pointer (&kept);
  }
  for (guint i = 0; i < bro_json_value_length (log); i++)
    {
      BroJsonValue *l = bro_json_value_at (log, i);
      g_string_append_printf (more, "\n    %s", S (l, "code", ""));
      if (bro_json_value_length (J (l, "params")) > 0)
        {
          g_autofree char *p = bro_scenario_json_text (J (l, "params"));
          g_string_append_printf (more, " %s", p);
        }
    }
  g_string_append (more, ")");
  g_free (run->failure);
  run->failure = g_string_free (more, FALSE);
  return FALSE;
}

gboolean
bro_scenario_expect_jobs (BroScenarioRun *run, BroJsonValue *want)
{
  g_autoptr (BroJsonValue) jobs = bro_scenario_run_all_jobs (run);
  BroJsonValue *count = J (want, "count");
  const char *keys[] = { "outcomes", "states", "kinds" }, *fields[] = { "outcome", "state", "kind" };
  if (!jobs)
    return FALSE;
  if (count)
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_integer (bro_json_value_length (jobs));
      if (!same (run, count, got, "jobs count"))
        return FALSE;
    }
  for (guint k = 0; k < G_N_ELEMENTS (keys); k++)
    {
      BroJsonValue *w = J (want, keys[k]);
      g_autoptr (BroJsonValue) got = NULL;
      g_autofree char *what = g_strconcat ("jobs ", keys[k], NULL);
      if (!w)
        continue;
      got = bro_json_value_new_array ();
      for (guint i = 0; i < bro_json_value_length (jobs); i++)
        {
          BroJsonValue *v = J (bro_json_value_at (jobs, i), fields[k]);
          bro_json_value_append (got, v ? bro_json_value_ref (v) : bro_json_value_new_null ());
        }
      if (!same (run, w, got, what))
        return FALSE;
    }
  return TRUE;
}

/* ---- files ---- */

static void
walk (BroScenarioRun *run, const char *dir, const char *rel, GPtrArray *files, GPtrArray *folders)
{
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GPtrArray) entries = bro_file_system_list (BRO_FILE_SYSTEM (run->root->fs), dir, &error);
  for (guint i = 0; entries && i < entries->len; i++)
    {
      const BroDirectoryEntry *e = entries->pdata[i];
      g_autofree char *r = *rel ? g_strconcat (rel, "/", e->name, NULL) : g_strdup (e->name);
      if (e->is_directory)
        {
          g_autofree char *sub = g_strconcat (dir, "/", e->name, NULL);
          g_ptr_array_add (folders, g_strdup (r));
          walk (run, sub, r, files, folders);
        }
      else
        g_ptr_array_add (files, g_strdup (r));
    }
}

static gboolean
hidden (const char *path)
{
  g_auto (GStrv) parts = g_strsplit (path, "/", -1);
  for (guint i = 0; parts[i]; i++)
    if (parts[i][0] == '.')
      return TRUE;
  return FALSE;
}

static gboolean
component_match (const char *p, const char *t)
{
  if (!*p)
    return !*t;
  if (*p == '*')
    {
      for (const char *x = t;; x++)
        {
          if (component_match (p + 1, x))
            return TRUE;
          if (!*x)
            return FALSE;
        }
    }
  if (*p == '?')
    return *t && component_match (p + 1, t + 1);
  return *t == *p && component_match (p + 1, t + 1);
}

static gboolean
glob_parts (char **pats, guint i, char **parts, guint j)
{
  if (!pats[i])
    return !parts[j];
  if (!strcmp (pats[i], "**"))
    {
      for (guint k = j;; k++)
        {
          if (glob_parts (pats, i + 1, parts, k))
            return TRUE;
          if (!parts[k])
            return FALSE;
        }
    }
  if (!parts[j])
    return FALSE;
  if (parts[j][0] == '.' && pats[i][0] != '.')
    return FALSE;
  return component_match (pats[i], parts[j]) && glob_parts (pats, i + 1, parts, j + 1);
}

static char *
trim_slash (const char *s)
{
  char *t = g_strdup (s);
  gsize n = strlen (t);
  while (n > 0 && t[n - 1] == '/')
    t[--n] = '\0';
  return t;
}

gboolean
bro_scenario_expect_glob (const char *pattern, const char *path)
{
  g_autofree char *p = trim_slash (pattern), *t = trim_slash (path);
  g_auto (GStrv) pats = g_strsplit (p, "/", -1);
  g_auto (GStrv) parts = g_strsplit (t, "/", -1);
  return glob_parts (pats, 0, parts, 0) && (!g_str_has_suffix (pattern, "/") || g_str_has_suffix (path, "/"));
}

gboolean
bro_scenario_expect_files (BroScenarioRun *run, BroJsonValue *want)
{
  const char *under_given = S (want, "under", NULL);
  g_autofree char *under_path = bro_scenario_path (under_given ? under_given : "");
  g_autofree char *under = trim_slash (under_path);
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) folders = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) all = g_ptr_array_new ();
  g_autofree char *where = under_given ? g_strconcat (" under ", under_given, NULL) : g_strdup ("");
  BroJsonValue *exactly = J (want, "exactly"), *include = J (want, "include"), *exclude = J (want, "exclude");
  if (exists (run, under))
    walk (run, under, "", files, folders);
  for (guint i = 0; i < files->len; i++)
    g_ptr_array_add (all, files->pdata[i]);
  for (guint i = 0; i < folders->len; i++)
    g_ptr_array_add (all, folders->pdata[i]);
  if (exactly && exactly->kind == BRO_JSON_VALUE_ARRAY)
    {
      /* The non-hidden files, and every non-hidden empty folder as name/ (a job that saved nothing leaves no folder). */
      g_autoptr (GPtrArray) listed = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GPtrArray) patterns = g_ptr_array_new ();
      gboolean ok = TRUE;
      for (guint i = 0; i < files->len; i++)
        if (!hidden (files->pdata[i]))
          g_ptr_array_add (listed, g_strdup (files->pdata[i]));
      for (guint i = 0; i < folders->len; i++)
        {
          const char *d = folders->pdata[i];
          g_autofree char *prefix = g_strconcat (d, "/", NULL);
          gboolean empty = TRUE;
          for (guint k = 0; k < all->len && empty; k++)
            empty = !g_str_has_prefix (all->pdata[k], prefix);
          if (!hidden (d) && empty)
            g_ptr_array_add (listed, g_strconcat (d, "/", NULL));
        }
      g_ptr_array_sort (listed, by_text);
      for (guint i = 0; i < exactly->items->len; i++)
        g_ptr_array_add (patterns, (gpointer) bro_json_value_get_string (exactly->items->pdata[i], ""));
      for (guint i = 0; i < listed->len && ok; i++)
        {
          gboolean any = FALSE;
          for (guint k = 0; k < patterns->len && !any; k++)
            any = bro_scenario_expect_glob (patterns->pdata[k], listed->pdata[i]);
          ok = any;
        }
      for (guint k = 0; k < patterns->len && ok; k++)
        {
          gboolean any = FALSE;
          for (guint i = 0; i < listed->len && !any; i++)
            any = bro_scenario_expect_glob (patterns->pdata[k], listed->pdata[i]);
          ok = any;
        }
      if (!ok)
        {
          g_autoptr (GPtrArray) sorted = g_ptr_array_copy (patterns, NULL, NULL);
          g_autofree char *w = NULL, *g = NULL, *expected = NULL;
          g_ptr_array_sort (sorted, by_text);
          w = show (sorted);
          g = show (listed);
          expected = g_strdup_printf ("exactly %s%s", w, where);
          return bro_scenario_run_fail (run, expected, g);
        }
    }
  for (guint i = 0; i < bro_json_value_length (include); i++)
    {
      const char *p = bro_json_value_get_string (bro_json_value_at (include, i), "");
      gboolean any = FALSE;
      for (guint k = 0; k < all->len && !any; k++)
        any = bro_scenario_expect_glob (p, all->pdata[k]);
      if (!any)
        {
          g_autoptr (GPtrArray) shown = g_ptr_array_new ();
          g_autofree char *expected = g_strconcat (p, where, NULL);
          g_autofree char *g = NULL;
          for (guint k = 0; k < all->len; k++)
            if (!hidden (all->pdata[k]) || strchr (p, '.'))
              g_ptr_array_add (shown, all->pdata[k]);
          g_ptr_array_sort (shown, by_text);
          g = show (shown);
          return bro_scenario_run_fail (run, expected, g);
        }
    }
  for (guint i = 0; i < bro_json_value_length (exclude); i++)
    {
      const char *p = bro_json_value_get_string (bro_json_value_at (exclude, i), "");
      g_autoptr (GPtrArray) found = g_ptr_array_new ();
      for (guint k = 0; k < all->len; k++)
        if (bro_scenario_expect_glob (p, all->pdata[k]))
          g_ptr_array_add (found, all->pdata[k]);
      if (found->len > 0)
        {
          g_autofree char *expected = g_strdup_printf ("no %s%s", p, where);
          g_autofree char *g = show (found);
          return bro_scenario_run_fail (run, expected, g);
        }
    }
  return TRUE;
}

gboolean
bro_scenario_expect_file (BroScenarioRun *run, BroJsonValue *want)
{
  const char *rel = S (want, "path", "");
  g_autofree char *path = bro_scenario_path (rel);
  BroJsonValue *there = J (want, "exists"), *v;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (BroFileInfo) info = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *text = NULL;
  gboolean only = TRUE;
  if (there && there->kind == BRO_JSON_VALUE_BOOL && exists (run, path) != there->boolean)
    {
      g_autofree char *expected = there->boolean ? g_strdup (rel) : g_strconcat ("no ", rel, NULL);
      return bro_scenario_run_fail (run, expected, exists (run, path) ? "it is there" : "it isn't there");
    }
  for (guint i = 0; i < want->keys->len; i++)
    only &= !strcmp (want->keys->pdata[i], "path") || !strcmp (want->keys->pdata[i], "exists");
  if (only)
    return TRUE;
  info = exists (run, path) ? bro_file_system_stat (BRO_FILE_SYSTEM (run->root->fs), path, &error) : NULL;
  if (!info || info->is_directory)
    {
      g_autofree char *expected = g_strconcat ("the file ", rel, NULL);
      return bro_scenario_run_fail (run, expected, "none");
    }
  bytes = read_file (run, path);
  text = text_of (bytes);
  if ((v = J (want, "text")))
    {
      const char *wanted = bro_json_value_get_string (v, "");
      g_autofree char *fixture = NULL;
      if (g_str_has_prefix (wanted, "<<shared/fixtures/") && g_str_has_suffix (wanted, ">>"))
        {
          g_autofree char *name = g_strndup (wanted + 18, strlen (wanted) - 20);
          fixture = bro_test_fixture_text (name, NULL);
        }
      if (g_strcmp0 (text, fixture ? fixture : wanted) != 0)
        {
          g_autofree char *expected = g_strdup_printf ("%s reading \"%s\"", rel, fixture ? fixture : wanted);
          g_autofree char *found = g_strdup_printf ("\"%s\"", text);
          return bro_scenario_run_fail (run, expected, found);
        }
    }
  v = J (want, "contains");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      if (!strstr (text, c))
        {
          g_autofree char *expected = g_strdup_printf ("%s containing \"%s\"", rel, c);
          g_autofree char *found = g_strdup_printf ("\"%s\"", text);
          return bro_scenario_run_fail (run, expected, found);
        }
    }
  v = J (want, "excludes");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      if (strstr (text, c))
        {
          g_autofree char *expected = g_strdup_printf ("%s without \"%s\"", rel, c);
          g_autofree char *found = g_strdup_printf ("\"%s\"", text);
          return bro_scenario_run_fail (run, expected, found);
        }
    }
  if ((v = J (want, "json")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_parse (g_bytes_get_data (bytes, NULL), (gssize) g_bytes_get_size (bytes));
      if (!same (run, v, got, rel))
        return FALSE;
    }
  if ((v = J (want, "sha256")))
    {
      g_autofree char *sha = bro_sha256_sums_hash (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
      if (g_strcmp0 (sha, bro_json_value_get_string (v, "")) != 0)
        {
          g_autofree char *expected = g_strdup_printf ("%s sha256 %s", rel, bro_json_value_get_string (v, ""));
          return bro_scenario_run_fail (run, expected, sha);
        }
    }
  if (J (want, "sameAsDisc"))
    return bro_scenario_run_fail (run, "file.sameAsDisc (phase 5)", "not built");
  return TRUE;
}

/* ---- sums, record, transcripts ---- */

gboolean
bro_scenario_expect_sums (BroScenarioRun *run, BroJsonValue *want)
{
  const char *rel = S (want, "folder", "");
  g_autofree char *folder = bro_scenario_path (rel);
  g_autofree char *sums = g_strconcat (folder, "/" BRO_SHA256_SUMS_FILE_NAME, NULL);
  g_autoptr (GBytes) bytes = NULL;
  g_autofree char *text = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  g_autoptr (GPtrArray) bad = g_ptr_array_new_with_free_func (g_free);
  BroJsonValue *ok = J (want, "ok"), *files = J (want, "files");
  if (!exists (run, sums))
    {
      g_autofree char *expected = g_strconcat (rel, "/" BRO_SHA256_SUMS_FILE_NAME, NULL);
      return bro_scenario_run_fail (run, expected, "none");
    }
  bytes = read_file (run, sums);
  text = text_of (bytes);
  entries = bro_sha256_sums_parse (text);
  for (guint i = 0; entries && i < entries->len; i++)
    {
      const BroSumEntry *e = entries->pdata[i];
      g_autofree char *path = g_strconcat (folder, "/", e->path, NULL);
      if (!exists (run, path))
        g_ptr_array_add (bad, g_strconcat (e->path, " missing", NULL));
      else
        {
          g_autoptr (GBytes) content = read_file (run, path);
          g_autoptr (BroSha256Hasher) hasher = bro_sha256_hasher_new ();
          g_autofree char *sha = NULL;
          if (content)
            bro_sha256_hasher_update (hasher, g_bytes_get_data (content, NULL), g_bytes_get_size (content));
          sha = bro_sha256_hasher_finish (hasher);
          if (g_strcmp0 (sha, e->sha256) != 0)
            g_ptr_array_add (bad, g_strconcat (e->path, " differs", NULL));
        }
    }
  if (ok && ok->kind == BRO_JSON_VALUE_BOOL && (bad->len == 0) != ok->boolean)
    {
      g_autofree char *expected = g_strdup_printf ("sha256sum -c in %s %s", rel, ok->boolean ? "passing" : "failing");
      g_autoptr (GString) found = g_string_new (bad->len ? "" : "it passes");
      for (guint i = 0; i < bad->len; i++)
        g_string_append_printf (found, "%s%s", i ? ", " : "", (char *) bad->pdata[i]);
      return bro_scenario_run_fail (run, expected, found->str);
    }
  if (files)
    {
      g_autoptr (GPtrArray) w = g_ptr_array_new ();
      g_autoptr (GPtrArray) g = g_ptr_array_new ();
      g_autoptr (BroJsonValue) wj = bro_json_value_new_array ();
      g_autoptr (BroJsonValue) gj = bro_json_value_new_array ();
      g_autofree char *what = g_strdup_printf ("the files %s/SHA256SUMS lists", rel);
      for (guint i = 0; i < bro_json_value_length (files); i++)
        g_ptr_array_add (w, (gpointer) bro_json_value_get_string (bro_json_value_at (files, i), ""));
      for (guint i = 0; entries && i < entries->len; i++)
        g_ptr_array_add (g, ((BroSumEntry *) entries->pdata[i])->path);
      g_ptr_array_sort (w, by_text);
      g_ptr_array_sort (g, by_text);
      for (guint i = 0; i < w->len; i++)
        bro_json_value_append (wj, bro_json_value_new_string (w->pdata[i]));
      for (guint i = 0; i < g->len; i++)
        bro_json_value_append (gj, bro_json_value_new_string (g->pdata[i]));
      if (!same (run, wj, gj, what))
        return FALSE;
    }
  return TRUE;
}

gboolean
bro_scenario_expect_record (BroScenarioRun *run, BroJsonValue *want, const char *named)
{
  const char *name = S (want, "job", named);
  g_autofree char *unit8 = NULL;
  g_autofree char *file = NULL;
  g_autofree char *rel = NULL;
  g_autofree char *path = NULL;
  g_autofree char *what = NULL;
  g_autoptr (BroJsonValue) wanted = NULL;
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  g_autoptr (BroJsonValue) got = NULL;
  g_autoptr (GBytes) bytes = NULL;
  BroId id;
  if (!name && !(name = bro_scenario_run_only_job (run)))
    return FALSE;
  if (!bro_scenario_run_job (run, name, &id))
    return FALSE;
  unit8 = unit8_of (&id);
  file = g_strconcat ("bromelia-", unit8, ".json", NULL);
  if (J (want, "folder"))
    {
      g_autoptr (BroJsonValue) folder = bro_scenario_expect_fill (run, J (want, "folder"), name);
      g_autofree char *base = NULL;
      if (!folder)
        return FALSE;
      rel = g_strdup (bro_json_value_get_string (folder, ""));
      base = bro_scenario_path (rel);
      path = g_strconcat (base, "/", file, NULL);
    }
  else
    {
      /* The unit's folder, wherever it went: the one record of that name under the root. */
      g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GPtrArray) folders = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GPtrArray) found = g_ptr_array_new ();
      g_autofree char *suffix = g_strconcat ("/", file, NULL);
      walk (run, TEST_ROOT, "", files, folders);
      for (guint i = 0; i < files->len; i++)
        if (!strcmp (files->pdata[i], file) || g_str_has_suffix (files->pdata[i], suffix))
          g_ptr_array_add (found, files->pdata[i]);
      if (found->len != 1)
        {
          g_autofree char *expected = g_strconcat ("one record ", file, NULL);
          g_autofree char *g = show (found);
          return bro_scenario_run_fail (run, expected, g);
        }
      rel = g_strdup (found->pdata[0]);
      path = g_strconcat (TEST_ROOT "/", rel, NULL);
    }
  if (!exists (run, path))
    {
      g_autofree char *expected = g_strconcat ("the record ", path, NULL);
      return bro_scenario_run_fail (run, expected, "none");
    }
  if (!(wanted = bro_scenario_expect_fill (run, J (want, "json") ? J (want, "json") : empty, name)))
    return FALSE;
  bytes = read_file (run, path);
  got = bytes ? bro_json_value_parse (g_bytes_get_data (bytes, NULL), (gssize) g_bytes_get_size (bytes)) : NULL;
  what = g_strconcat ("the record in ", rel, NULL);
  return same (run, wanted, got, what);
}

static gboolean
transcript (BroScenarioRun *run, BroJsonValue *want, const char *named, const char *file, const char *what)
{
  const char *name = S (want, "job", named);
  g_autofree char *path = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autofree char *text = NULL;
  BroJsonValue *runs = J (want, "runs"), *v;
  BroId id;
  gsize at = 0;
  if (!name && !(name = bro_scenario_run_only_job (run)))
    return FALSE;
  if (!bro_scenario_run_job (run, name, &id))
    return FALSE;
  path = g_strconcat (TEST_ROOT_DATA_DIR "/jobs/", id.value, "/", file, NULL);
  if (!exists (run, path))
    {
      g_autofree char *expected = g_strdup_printf ("the %s of %s", what, name);
      return bro_scenario_run_fail (run, expected, "none");
    }
  bytes = read_file (run, path);
  text = text_of (bytes);
  if (runs && runs->kind == BRO_JSON_VALUE_INTEGER)
    {
      g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
      gint64 found = 0;
      for (guint i = 0; lines[i]; i++)
        found += g_str_has_prefix (lines[i], "==== ") && strstr (lines[i], " $ ") != NULL;
      if (found != runs->integer)
        {
          g_autofree char *expected = g_strdup_printf ("%" G_GINT64_FORMAT " runs in the %s of %s", runs->integer, what, name);
          return bro_scenario_run_failf (run, expected, "%" G_GINT64_FORMAT ":\n%s", found, text);
        }
    }
  v = J (want, "contains");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      if (!strstr (text, c))
        {
          g_autofree char *expected = g_strdup_printf ("the %s of %s containing \"%s\"", what, name, c);
          return bro_scenario_run_fail (run, expected, text);
        }
    }
  v = J (want, "order");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      const char *found = strstr (text + at, c);
      if (!found)
        {
          g_autofree char *expected = g_strdup_printf ("the %s of %s containing \"%s\" after what came before", what, name, c);
          return bro_scenario_run_fail (run, expected, text);
        }
      at = (gsize) (found - text) + strlen (c);
    }
  return TRUE;
}

gboolean
bro_scenario_expect_transcript (BroScenarioRun *run, BroJsonValue *want, const char *named)
{
  return transcript (run, want, named, BRO_SEAL_STEP_TRANSCRIPT, "transcript");
}

gboolean
bro_scenario_expect_debug_log (BroScenarioRun *run, BroJsonValue *want, const char *named)
{
  return transcript (run, want, named, BRO_SEAL_STEP_DEBUG_TRANSCRIPT, "debug log");
}

/* ---- calls, log, events, problems, drives, config ---- */

gboolean
bro_scenario_expect_calls (BroScenarioRun *run, BroJsonValue *want)
{
  for (guint t = 0; t < want->keys->len; t++)
    {
      const char *tool = want->keys->pdata[t];
      BroJsonValue *w = want->items->pdata[t], *count = J (w, "count"), *v;
      g_autoptr (GPtrArray) calls = bro_scenario_tools_calls (run->tools, tool);
      g_autoptr (GString) found = g_string_new (NULL);
      g_string_append_printf (found, "%u calls: ", calls->len);
      for (guint i = 0; i < calls->len; i++)
        g_string_append_printf (found, "%s%s", i ? "\n  " : "", (char *) calls->pdata[i]);
      if (bro_json_value_get_bool (J (w, "none"), FALSE) && calls->len > 0)
        {
          g_autofree char *expected = g_strdup_printf ("no %s call", tool);
          return bro_scenario_run_fail (run, expected, found->str);
        }
      if (count && count->kind == BRO_JSON_VALUE_INTEGER && calls->len != (guint) count->integer)
        {
          g_autofree char *expected = g_strdup_printf ("%" G_GINT64_FORMAT " %s calls", count->integer, tool);
          return bro_scenario_run_fail (run, expected, found->str);
        }
      v = J (w, "include");
      for (guint i = 0; i < bro_json_value_length (v); i++)
        {
          const char *s = bro_json_value_get_string (bro_json_value_at (v, i), "");
          gboolean any = FALSE;
          for (guint k = 0; k < calls->len && !any; k++)
            any = strstr (calls->pdata[k], s) != NULL;
          if (!any)
            {
              g_autofree char *expected = g_strdup_printf ("a %s call with \"%s\"", tool, s);
              return bro_scenario_run_fail (run, expected, found->str);
            }
        }
      v = J (w, "exclude");
      for (guint i = 0; i < bro_json_value_length (v); i++)
        {
          const char *s = bro_json_value_get_string (bro_json_value_at (v, i), "");
          for (guint k = 0; k < calls->len; k++)
            if (strstr (calls->pdata[k], s))
              {
                g_autofree char *expected = g_strdup_printf ("no %s call with \"%s\"", tool, s);
                return bro_scenario_run_fail (run, expected, found->str);
              }
        }
      v = J (w, "counts");
      for (guint i = 0; v && v->kind == BRO_JSON_VALUE_OBJECT && i < v->keys->len; i++)
        {
          const char *s = v->keys->pdata[i];
          gint64 n = bro_json_value_get_integer (v->items->pdata[i], -1), got = 0;
          for (guint k = 0; k < calls->len; k++)
            got += strstr (calls->pdata[k], s) != NULL;
          if (got != n)
            {
              g_autofree char *expected = g_strdup_printf ("%" G_GINT64_FORMAT " %s calls with \"%s\"", n, tool, s);
              return bro_scenario_run_fail (run, expected, found->str);
            }
        }
    }
  return TRUE;
}

gboolean
bro_scenario_expect_log (BroScenarioRun *run, BroJsonValue *want, const char *named)
{
  const char *name = S (want, "job", named);
  g_autoptr (BroJsonValue) log = NULL;
  g_autoptr (GPtrArray) codes = g_ptr_array_new ();
  g_autofree char *shown = NULL;
  BroJsonValue *v;
  BroId id;
  if (!name && !(name = bro_scenario_run_only_job (run)))
    return FALSE;
  if (!bro_scenario_run_job (run, name, &id) || !(log = bro_scenario_engine_job_log (run, &id)))
    return FALSE;
  for (guint i = 0; i < bro_json_value_length (log); i++)
    g_ptr_array_add (codes, (gpointer) S (bro_json_value_at (log, i), "code", ""));
  shown = show (codes);
  v = J (want, "include");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      if (!contains_string (codes, c))
        {
          g_autofree char *expected = g_strdup_printf ("%s in the log of %s", c, name);
          return bro_scenario_run_fail (run, expected, shown);
        }
    }
  v = J (want, "exclude");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      if (contains_string (codes, c))
        {
          g_autofree char *expected = g_strdup_printf ("no %s in the log of %s", c, name);
          return bro_scenario_run_fail (run, expected, shown);
        }
    }
  return TRUE;
}

/* An event as an expectation reads it: its data's members and its type. */
static BroJsonValue *
event_json (BroJsonValue *e)
{
  BroJsonValue *data = J (e, "data"), *o = bro_json_value_new_object ();
  for (guint i = 0; data && data->kind == BRO_JSON_VALUE_OBJECT && i < data->keys->len; i++)
    bro_json_value_set (o, data->keys->pdata[i], bro_json_value_ref (data->items->pdata[i]));
  bro_json_value_set (o, "type", bro_json_value_new_string (S (e, "type", "")));
  return o;
}

gboolean
bro_scenario_expect_events (BroScenarioRun *run, BroJsonValue *want)
{
  g_autoptr (BroJsonValue) events = bro_scenario_run_events (run);
  g_autoptr (GPtrArray) types = g_ptr_array_new ();
  g_autofree char *shown = NULL;
  BroJsonValue *v;
  guint at = 0;
  for (guint i = 0; i < bro_json_value_length (events); i++)
    g_ptr_array_add (types, (gpointer) S (bro_json_value_at (events, i), "type", ""));
  shown = show (types);
  v = J (want, "include");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      BroJsonValue *w = bro_json_value_at (v, i);
      gboolean any = FALSE;
      for (guint k = 0; k < bro_json_value_length (events) && !any; k++)
        {
          g_autoptr (BroJsonValue) e = event_json (bro_json_value_at (events, k));
          any = bro_scenario_expect_matches (w, e);
        }
      if (!any)
        {
          g_autofree char *text = bro_scenario_json_text (w);
          g_autofree char *expected = g_strconcat ("an event ", text, NULL);
          return bro_scenario_run_fail (run, expected, shown);
        }
    }
  v = J (want, "sequence");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *t = bro_json_value_get_string (bro_json_value_at (v, i), "");
      guint k = at;
      while (k < types->len && strcmp (types->pdata[k], t) != 0)
        k++;
      if (k >= types->len)
        {
          g_autofree char *seq = bro_scenario_json_text (v);
          g_autofree char *expected = g_strdup_printf ("%s in the sequence %s", t, seq);
          return bro_scenario_run_fail (run, expected, shown);
        }
      at = k + 1;
    }
  v = J (want, "exclude");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *t = bro_json_value_get_string (bro_json_value_at (v, i), "");
      if (contains_string (types, t))
        {
          g_autofree char *expected = g_strconcat ("no ", t, NULL);
          return bro_scenario_run_fail (run, expected, shown);
        }
    }
  return TRUE;
}

gboolean
bro_scenario_expect_problems (BroScenarioRun *run, BroJsonValue *want)
{
  g_autoptr (BroJsonValue) system = bro_scenario_engine_system (run);
  g_autoptr (GPtrArray) active = g_ptr_array_new ();
  g_autofree char *shown = NULL;
  BroJsonValue *problems, *v;
  if (!system)
    return FALSE;
  problems = J (system, "problems");
  for (guint i = 0; i < bro_json_value_length (problems); i++)
    g_ptr_array_add (active, (gpointer) S (bro_json_value_at (problems, i), "code", ""));
  shown = show (active);
  if (bro_json_value_get_bool (J (want, "none"), FALSE) && active->len > 0)
    return bro_scenario_run_fail (run, "no problems", shown);
  v = J (want, "active");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      if (!contains_string (active, c))
        {
          g_autofree char *expected = g_strconcat ("the problem ", c, NULL);
          return bro_scenario_run_fail (run, expected, shown);
        }
    }
  v = J (want, "cleared");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      const char *c = bro_json_value_get_string (bro_json_value_at (v, i), "");
      g_autoptr (BroJsonValue) events = bro_scenario_run_events (run);
      g_autofree char *quoted = g_strdup_printf ("\"%s\"", c);
      gboolean found = FALSE;
      if (contains_string (active, c))
        {
          g_autofree char *expected = g_strconcat (c, " cleared", NULL);
          return bro_scenario_run_fail (run, expected, "still active");
        }
      for (guint k = 0; k < bro_json_value_length (events) && !found; k++)
        {
          BroJsonValue *e = bro_json_value_at (events, k);
          g_autofree char *text = NULL;
          if (strcmp (S (e, "type", ""), "problemCleared") != 0)
            continue;
          text = bro_json_value_to_text (J (e, "data"));
          found = strstr (text, quoted) != NULL;
        }
      if (!found)
        {
          g_autofree char *expected = g_strconcat ("a problemCleared for ", c, NULL);
          return bro_scenario_run_fail (run, expected, "none");
        }
    }
  return TRUE;
}

gboolean
bro_scenario_expect_drives (BroScenarioRun *run, BroJsonValue *want)
{
  g_autoptr (BroJsonValue) drives = bro_scenario_engine_drives (run);
  if (!drives)
    return FALSE;
  for (guint i = 0; i < bro_json_value_length (want); i++)
    {
      BroJsonValue *w = bro_json_value_at (want, i), *drive = NULL;
      const char *id = S (w, "id", "");
      const char *device = NULL;
      g_autoptr (BroJsonValue) subset = bro_json_value_new_object ();
      g_autofree char *what = g_strconcat ("the drive ", id, NULL);
      for (guint k = 0; k < run->world->drives->len; k++)
        if (!strcmp (((BroScenarioDrive *) run->world->drives->pdata[k])->name, id))
          device = ((BroScenarioDrive *) run->world->drives->pdata[k])->device;
      for (guint k = 0; k < bro_json_value_length (drives) && !drive; k++)
        {
          BroJsonValue *d = bro_json_value_at (drives, k);
          if (device ? !g_strcmp0 (S (d, "device", NULL), device) : !g_strcmp0 (S (d, "configId", NULL), id))
            drive = d;
        }
      if (!drive)
        {
          g_autofree char *text = bro_scenario_json_text (drives);
          return bro_scenario_run_fail (run, what, text);
        }
      for (guint k = 0; k < w->keys->len; k++)
        if (strcmp (w->keys->pdata[k], "id") != 0)
          bro_json_value_set (subset, w->keys->pdata[k], bro_json_value_ref (w->items->pdata[k]));
      if (!same (run, subset, drive, what))
        return FALSE;
    }
  return TRUE;
}

gboolean
bro_scenario_expect_config (BroScenarioRun *run, BroJsonValue *want)
{
  g_autoptr (BroJsonValue) envelope = bro_scenario_engine_config (run);
  return envelope && same (run, want, J (envelope, "doc"), "the configuration");
}

/* ---- the step ---- */

static gboolean
filled (BroScenarioRun *run, BroJsonValue *want, const char *named, gboolean (*check) (BroScenarioRun *, BroJsonValue *))
{
  g_autoptr (BroJsonValue) v = bro_scenario_expect_fill (run, want, named);
  return v && check (run, v);
}

static gboolean
filled_named (BroScenarioRun *run, BroJsonValue *want, const char *named, Keyed check)
{
  g_autoptr (BroJsonValue) v = bro_scenario_expect_fill (run, want, named);
  return v && check (run, v, named);
}

gboolean
bro_scenario_expect_check (BroScenarioRun *run, BroJsonValue *expect)
{
  const char *named = S (J (expect, "job"), "name", NULL);
  BroJsonValue *when = J (expect, "when");
  if (when ? !bro_scenario_expect_when (run, when) : !bro_scenario_run_settle (run))
    return FALSE;
  for (guint i = 0; i < expect->keys->len; i++)
    {
      const char *key = expect->keys->pdata[i];
      BroJsonValue *want = expect->items->pdata[i];
      gboolean ok;
      if (!strcmp (key, "when") || !strcmp (key, "note"))
        continue;
      if (!strcmp (key, "job"))
        ok = filled (run, want, named, bro_scenario_expect_job);
      else if (!strcmp (key, "jobs"))
        ok = bro_scenario_expect_jobs (run, want);
      else if (!strcmp (key, "files"))
        ok = filled (run, want, named, bro_scenario_expect_files);
      else if (!strcmp (key, "file"))
        ok = filled (run, want, named, bro_scenario_expect_file);
      else if (!strcmp (key, "sums"))
        ok = filled (run, want, named, bro_scenario_expect_sums);
      else if (!strcmp (key, "record"))
        ok = bro_scenario_expect_record (run, want, named);
      else if (!strcmp (key, "transcript"))
        ok = filled_named (run, want, named, bro_scenario_expect_transcript);
      else if (!strcmp (key, "debugLog"))
        ok = filled_named (run, want, named, bro_scenario_expect_debug_log);
      else if (!strcmp (key, "calls"))
        ok = filled (run, want, named, bro_scenario_expect_calls);
      else if (!strcmp (key, "log"))
        ok = filled_named (run, want, named, bro_scenario_expect_log);
      else if (!strcmp (key, "events"))
        ok = filled (run, want, named, bro_scenario_expect_events);
      else if (!strcmp (key, "problems"))
        ok = filled (run, want, named, bro_scenario_expect_problems);
      else if (!strcmp (key, "drives"))
        ok = filled (run, want, named, bro_scenario_expect_drives);
      else if (!strcmp (key, "db"))
        ok = filled (run, want, named, bro_scenario_expect_db);
      else if (!strcmp (key, "config"))
        ok = filled (run, want, named, bro_scenario_expect_config);
      else if (!strcmp (key, "manifest") || !strcmp (key, "notifications"))
        {
          g_autofree char *what = g_strdup_printf ("%s (phase 5)", key);
          ok = bro_scenario_run_fail (run, what, "not built");
        }
      else if (!strcmp (key, "session") || !strcmp (key, "power") || !strcmp (key, "response"))
        {
          g_autofree char *what = g_strdup_printf ("%s (phase 4)", key);
          ok = bro_scenario_run_fail (run, what, "not built");
        }
      else
        ok = bro_scenario_run_fail (run, "a known expectation", key);
      if (!ok)
        return FALSE;
    }
  return TRUE;
}
