/* test-store.inc: SqliteStore, included by test-adapters.c. shared/fixtures/adapters/store.cases.json, and every
 * record type read back equal. */

/* A clock stopped at 2026-10-04T12:00:00.000Z. */
#define TEST_TYPE_FIXED_CLOCK (test_fixed_clock_get_type ())
G_DECLARE_FINAL_TYPE (TestFixedClock, test_fixed_clock, TEST, FIXED_CLOCK, GObject)
struct _TestFixedClock { GObject parent_instance; };
static void test_fixed_clock_iface_init (BroClockInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestFixedClock, test_fixed_clock, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, test_fixed_clock_iface_init))
static BroInstant fc_now (BroClock *c) { BroInstant t; bro_instant_parse ("2026-10-04T12:00:00.000Z", &t); return t; }
static BroDuration fc_mono (BroClock *c) { return (BroDuration) { 0 }; }
static void test_fixed_clock_class_init (TestFixedClockClass *k) {}
static void test_fixed_clock_init (TestFixedClock *self) {}
static void test_fixed_clock_iface_init (BroClockInterface *iface) { iface->now = fc_now; iface->monotonic = fc_mono; }

static char *store_dir;

static BroSqliteStore *
new_store (const char *name)
{
  g_autoptr (TestFixedClock) clock = g_object_new (TEST_TYPE_FIXED_CLOCK, NULL);
  g_autofree char *path = g_strdup_printf ("%s/%s.sqlite", store_dir, name);
  g_autoptr (BroBroError) error = NULL;
  BroSqliteStore *store = bro_sqlite_store_open (path, BRO_CLOCK (clock), &error);
  g_assert_null (error);
  g_assert_true (bro_sqlite_store_migrate (store, &error));
  return store;
}

static sqlite3 *
raw_db (const char *name)
{
  g_autofree char *path = g_strdup_printf ("%s/%s.sqlite", store_dir, name);
  sqlite3 *db = NULL;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  sqlite3_exec (db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
  return db;
}

static gboolean
seed (sqlite3 *db, BroJsonValue *rows, GString *problem)
{
  for (guint t = 0; t < bro_json_value_length (rows); t++)
    {
      const char *table = rows->keys->pdata[t];
      BroJsonValue *list = bro_json_value_at (rows, t);
      for (guint r = 0; r < bro_json_value_length (list); r++)
        {
          BroJsonValue *row = bro_json_value_at (list, r);
          g_autoptr (GString) sql = g_string_new (NULL);
          sqlite3_stmt *stmt = NULL;
          g_string_printf (sql, "INSERT INTO %s (", table);
          for (guint c = 0; c < bro_json_value_length (row); c++)
            g_string_append_printf (sql, "%s%s", c ? ", " : "", (char *) row->keys->pdata[c]);
          g_string_append (sql, ") VALUES (");
          for (guint c = 0; c < bro_json_value_length (row); c++)
            g_string_append (sql, c ? ", ?" : "?");
          g_string_append (sql, ")");
          if (sqlite3_prepare_v2 (db, sql->str, -1, &stmt, NULL) != SQLITE_OK)
            {
              g_string_append (problem, sqlite3_errmsg (db));
              return FALSE;
            }
          for (guint c = 0; c < bro_json_value_length (row); c++)
            {
              BroJsonValue *v = bro_json_value_at (row, c);
              int idx = (int) c + 1;
              switch (v->kind)
                {
                case BRO_JSON_VALUE_NULL: sqlite3_bind_null (stmt, idx); break;
                case BRO_JSON_VALUE_STRING:
                  if (g_str_equal (table, "lookup_cache") && g_str_equal (row->keys->pdata[c], "response"))
                    sqlite3_bind_blob (stmt, idx, v->string, (int) strlen (v->string), SQLITE_TRANSIENT);
                  else
                    sqlite3_bind_text (stmt, idx, v->string, -1, SQLITE_TRANSIENT);
                  break;
                case BRO_JSON_VALUE_INTEGER: sqlite3_bind_int64 (stmt, idx, v->integer); break;
                case BRO_JSON_VALUE_NUMBER: sqlite3_bind_double (stmt, idx, v->number); break;
                case BRO_JSON_VALUE_BOOL: sqlite3_bind_int64 (stmt, idx, v->boolean ? 1 : 0); break;
                default: break;
                }
            }
          if (sqlite3_step (stmt) != SQLITE_DONE)
            {
              g_string_append (problem, sqlite3_errmsg (db));
              sqlite3_finalize (stmt);
              return FALSE;
            }
          sqlite3_finalize (stmt);
        }
    }
  return TRUE;
}

static BroJsonValue *
ids_of (GPtrArray *items, const char *(*id) (gpointer))
{
  BroJsonValue *v = bro_json_value_new_array ();
  for (guint i = 0; items && i < items->len; i++)
    bro_json_value_append (v, bro_json_value_new_string (id (items->pdata[i])));
  return v;
}

static const char *job_id_of (gpointer p) { return ((BroJobRecord *) p)->id.value; }
static const char *unit_id_of (gpointer p) { return ((BroUnitRecord *) p)->id.value; }
static const char *work_id_of (gpointer p) { return ((BroWorkRecord *) p)->id.value; }
static const char *check_id_of (gpointer p) { return ((BroCheckRecord *) p)->id.value; }
static const char *replica_id_of (gpointer p) { return ((BroReplicaRecord *) p)->id.value; }
static const char *outbox_id_of (gpointer p) { return ((BroOutboxEntry *) p)->id.value; }

static BroId
id_from (const char *text)
{
  BroId id = { { 0 } };
  g_strlcpy (id.value, text ? text : "", sizeof id.value);
  return id;
}

static BroContinuationQuery *
continuation (BroJsonValue *a)
{
  BroContinuationQuery *q = bro_continuation_query_new (bro_json_value_get_string (bro_json_value_member (a, "name"), ""),
                                                        bro_json_value_get_string (bro_json_value_member (a, "labelTitle"), ""),
                                                        (int) bro_json_value_get_integer (bro_json_value_member (a, "disc"), 0));
  q->season = (int) bro_json_value_get_integer (bro_json_value_member (a, "season"), -1);
  q->part = (int) bro_json_value_get_integer (bro_json_value_member (a, "part"), -1);
  q->volume = (int) bro_json_value_get_integer (bro_json_value_member (a, "volume"), -1);
  return q;
}

static BroJsonValue *
int_or_null (int v)
{
  return v >= 0 ? bro_json_value_new_integer (v) : bro_json_value_new_null ();
}

static int store_case_number;

static gboolean
store_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autofree char *name = g_strdup_printf ("case%d", store_case_number++);
  g_autoptr (BroSqliteStore) store = new_store (name);
  sqlite3 *db = raw_db (name);
  g_autoptr (GString) problem = g_string_new (NULL);
  g_autoptr (BroBroError) error = NULL;
  BroStore *s = BRO_STORE (store);
  BroJsonValue *a = bro_json_value_member (given, "args");
  const char *call = bro_json_value_get_string (bro_json_value_member (given, "call"), "");
  const char *key = "ids";
  g_autoptr (BroJsonValue) got = NULL;
#define ARG_S(k) bro_json_value_get_string (bro_json_value_member (a, k), NULL)
#define ARG_I(k) bro_json_value_get_integer (bro_json_value_member (a, k), -1)
  if (!seed (db, bro_json_value_member (given, "rows"), problem))
    {
      bro_test_fail (failures, id, "seed: %s", problem->str);
      sqlite3_close (db);
      return TRUE;
    }
  if (g_str_equal (call, "jobs.active"))
    {
      g_autoptr (GPtrArray) r = bro_job_repository_active (bro_store_jobs (s), &error);
      got = ids_of (r, job_id_of);
    }
  else if (g_str_equal (call, "jobs.query"))
    {
      g_autoptr (BroJobQuery) q = bro_job_query_new ();
      g_autoptr (GPtrArray) r = NULL;
      q->has_state = ARG_S ("state") && bro_job_state_from_wire (ARG_S ("state"), &q->state);
      q->has_kind = ARG_S ("kind") && bro_job_kind_from_wire (ARG_S ("kind"), &q->kind);
      q->has_finished_after = ARG_S ("finishedAfter") && bro_instant_parse (ARG_S ("finishedAfter"), &q->finished_after);
      q->limit = (int) ARG_I ("limit");
      q->offset = (int) ARG_I ("offset");
      r = bro_job_repository_query (bro_store_jobs (s), q, &error);
      got = ids_of (r, job_id_of);
    }
  else if (g_str_equal (call, "steps.completed") || g_str_equal (call, "steps.load"))
    {
      g_autoptr (GPtrArray) r = g_str_equal (call, "steps.load") ? bro_step_repository_load (bro_store_steps (s), id_from (ARG_S ("jobId")), &error)
                                                                 : bro_step_repository_completed (bro_store_steps (s), id_from (ARG_S ("jobId")), &error);
      got = bro_json_value_new_array ();
      for (guint i = 0; r && i < r->len; i++)
        bro_json_value_append (got, bro_json_value_new_integer (((BroStepRecord *) r->pdata[i])->seq));
      key = "seqs";
    }
  else if (g_str_equal (call, "units.query"))
    {
      g_autoptr (BroUnitQuery) q = bro_unit_query_new ();
      g_autoptr (GPtrArray) r = NULL;
      q->library_id = g_strdup (ARG_S ("libraryId"));
      q->text = g_strdup (ARG_S ("text"));
      q->has_state = ARG_S ("state") && bro_unit_state_from_wire (ARG_S ("state"), &q->state);
      q->limit = (int) ARG_I ("limit");
      q->offset = (int) ARG_I ("offset");
      r = bro_unit_repository_query (bro_store_units (s), q, &error);
      got = ids_of (r, unit_id_of);
    }
  else if (g_str_equal (call, "units.leastRecentlyVerified"))
    {
      g_autoptr (GPtrArray) r = bro_unit_repository_least_recently_verified (bro_store_units (s), (int) ARG_I ("limit"), &error);
      got = ids_of (r, unit_id_of);
    }
  else if (g_str_equal (call, "units.openIntents"))
    {
      g_autoptr (GPtrArray) r = bro_unit_repository_open_intents (bro_store_units (s), &error);
      got = bro_json_value_new_array ();
      for (guint i = 0; r && i < r->len; i++)
        {
          BroCommitIntent *intent = r->pdata[i];
          BroJsonValue *pair = bro_json_value_new_array ();
          BroJsonValue *items = bro_json_value_new_array ();
          for (guint k = 0; k < intent->items->len; k++)
            {
              BroCommitItem *it = intent->items->pdata[k];
              g_autofree char *line = g_strdup_printf ("%d %s %s%s%s", it->seq, it->from_path, it->to_path, it->is_dir ? " dir" : "",
                                                       it->moved ? " moved" : "");
              bro_json_value_append (items, bro_json_value_new_string (line));
            }
          bro_json_value_append (pair, bro_json_value_new_string (intent->unit_id.value));
          bro_json_value_append (pair, items);
          bro_json_value_append (got, pair);
        }
      key = "intents";
    }
  else if (g_str_equal (call, "catalog.archivedBefore"))
    {
      g_autoptr (GPtrArray) r = bro_catalog_repository_archived_before (bro_store_catalog (s), ARG_S ("fingerprint"), &error);
      got = ids_of (r, unit_id_of);
    }
  else if (g_str_equal (call, "catalog.previousDisc"))
    {
      g_autoptr (BroContinuationQuery) q = continuation (a);
      BroArchivedDisc *p = bro_catalog_repository_previous_disc (bro_store_catalog (s), q, &error);
      if (p)
        {
          got = bro_json_value_new_object ();
          bro_json_value_set (got, "name", bro_json_value_new_string (p->name));
          bro_json_value_set (got, "disc", int_or_null (p->disc));
          bro_json_value_set (got, "lastEpisode", int_or_null (p->last_episode));
          bro_json_value_set (got, "folder", bro_json_value_new_string (p->folder));
          bro_archived_disc_free (p);
        }
      else
        got = bro_json_value_new_null ();
      key = "previous";
    }
  else if (g_str_equal (call, "catalog.highestEpisode"))
    {
      g_autoptr (BroContinuationQuery) q = continuation (a);
      got = int_or_null (bro_catalog_repository_highest_episode (bro_store_catalog (s), q, &error));
      key = "episode";
    }
  else if (g_str_equal (call, "catalog.works"))
    {
      g_autoptr (GPtrArray) r = bro_catalog_repository_works (bro_store_catalog (s), ARG_S ("query"), &error);
      got = ids_of (r, work_id_of);
    }
  else if (g_str_equal (call, "catalog.set"))
    {
      g_autoptr (BroDiscSetRecord) set = bro_catalog_repository_set (bro_store_catalog (s), id_from (ARG_S ("setId")), &error);
      got = bro_json_value_new_object ();
      bro_json_value_set (got, "workId", set && set->has_work_id ? bro_json_value_new_string (set->work_id.value) : bro_json_value_new_null ());
      bro_json_value_set (got, "labelTitle", bro_json_value_new_string (set ? set->label_title : ""));
      bro_json_value_set (got, "season", int_or_null (set && set->has_season ? set->season : -1));
      bro_json_value_set (got, "part", int_or_null (set && set->has_part ? set->part : -1));
      bro_json_value_set (got, "volume", int_or_null (set && set->has_volume ? set->volume : -1));
      bro_json_value_set (got, "description", bro_json_value_new_string (set ? set->description : ""));
      bro_json_value_set (got, "knownCount", int_or_null (set && set->has_known_count ? set->known_count : -1));
      key = "set";
    }
  else if (g_str_equal (call, "checks.forUnit"))
    {
      g_autoptr (GPtrArray) r = bro_check_repository_for_unit (bro_store_checks (s), id_from (ARG_S ("unitId")), &error);
      g_autoptr (BroCheckRecord) latest = bro_check_repository_latest (bro_store_checks (s),
                                                                         bro_json_value_get_string (bro_json_value_member (expect, "latestFolder"), ""), NULL);
      g_autoptr (BroJsonValue) changed = bro_json_value_new_array ();
      for (guint i = 0; r && r->len && ((BroCheckRecord *) r->pdata[0])->changed[i]; i++)
        bro_json_value_append (changed, bro_json_value_new_string (((BroCheckRecord *) r->pdata[0])->changed[i]));
      bro_test_same_json (failures, id, "changed", bro_json_value_member (expect, "changed"), changed);
      bro_test_same_string (failures, id, "latest", bro_json_value_get_string (bro_json_value_member (expect, "latest"), ""),
                            latest ? latest->id.value : NULL);
      got = ids_of (r, check_id_of);
    }
  else if (g_str_equal (call, "replicas.lagging"))
    {
      g_autoptr (GPtrArray) r = bro_replica_repository_lagging (bro_store_replicas (s), &error);
      got = ids_of (r, replica_id_of);
    }
  else if (g_str_equal (call, "outbox.due"))
    {
      BroInstant now = { 0 };
      g_autoptr (GPtrArray) r = NULL;
      bro_instant_parse (ARG_S ("now"), &now);
      r = bro_outbox_repository_due (bro_store_outbox (s), now, &error);
      got = ids_of (r, outbox_id_of);
    }
  else if (g_str_equal (call, "lookupCache.get"))
    {
      BroJsonValue *keys = bro_json_value_member (a, "keys");
      got = bro_json_value_new_array ();
      for (guint i = 0; i < bro_json_value_length (keys); i++)
        {
          BroHttpResponse *h = bro_lookup_cache_repository_get (bro_store_lookup_cache (s), ARG_S ("provider"),
                                                                bro_json_value_get_string (bro_json_value_at (keys, i), ""), &error);
          if (h)
            {
              gsize n = 0;
              const char *data = g_bytes_get_data (h->body, &n);
              g_autofree char *text = g_strndup (data, n);
              bro_json_value_append (got, bro_json_value_new_string (text));
              bro_http_response_free (h);
            }
          else
            bro_json_value_append (got, bro_json_value_new_null ());
        }
      key = "bodies";
    }
  else if (g_str_equal (call, "drives.recordStats"))
    {
      const char *parts[] = { "twice", "then" };
      sqlite3_stmt *stmt = NULL;
      for (int i = 0; i < 2; i++)
        {
          BroJsonValue *v = bro_json_value_member (a, parts[i]);
          BroDriveStats st = { (int) bro_json_value_get_integer (bro_json_value_member (v, "jobs"), 0),
                               (int) bro_json_value_get_integer (bro_json_value_member (v, "failedJobs"), 0),
                               (int) bro_json_value_get_integer (bro_json_value_member (v, "readErrorJobs"), 0),
                               (int) bro_json_value_get_integer (bro_json_value_member (v, "readErrors"), 0),
                               bro_json_value_get_integer (bro_json_value_member (v, "bytesRead"), 0),
                               bro_json_value_get_number (bro_json_value_member (v, "secondsReading"), 0) };
          bro_drive_repository_record_stats (bro_store_drives (s), ARG_S ("driveId"), ARG_S ("day"), &st, &error);
        }
      sqlite3_prepare_v2 (db, "SELECT jobs, failed_jobs, read_error_jobs, read_errors, bytes_read, seconds_reading FROM drive_stats_daily", -1, &stmt, NULL);
      got = bro_json_value_new_object ();
      if (sqlite3_step (stmt) == SQLITE_ROW)
        {
          bro_json_value_set (got, "jobs", bro_json_value_new_integer (sqlite3_column_int64 (stmt, 0)));
          bro_json_value_set (got, "failed_jobs", bro_json_value_new_integer (sqlite3_column_int64 (stmt, 1)));
          bro_json_value_set (got, "read_error_jobs", bro_json_value_new_integer (sqlite3_column_int64 (stmt, 2)));
          bro_json_value_set (got, "read_errors", bro_json_value_new_integer (sqlite3_column_int64 (stmt, 3)));
          bro_json_value_set (got, "bytes_read", bro_json_value_new_integer (sqlite3_column_int64 (stmt, 4)));
          bro_json_value_set (got, "seconds_reading", bro_json_value_new_number (sqlite3_column_double (stmt, 5)));
        }
      sqlite3_finalize (stmt);
      key = "row";
    }
  else
    {
      sqlite3_close (db);
      return FALSE;
    }
#undef ARG_S
#undef ARG_I
  if (error)
    bro_test_fail (failures, id, "error %s", error->code);
  bro_test_same_json (failures, id, key, bro_json_value_member (expect, key), got);
  sqlite3_close (db);
  return TRUE;
}

static void
test_store_cases (void)
{
  bro_test_run_cases ("adapters/store.cases.json", store_case);
}

/* ---- every record reads back equal ---- */

static BroInstant
at (const char *text)
{
  BroInstant t = { 0 };
  bro_instant_parse (text, &t);
  return t;
}

static BroId
u (int n)
{
  BroId id;
  g_snprintf (id.value, sizeof id.value, "00000000-0000-4000-8000-%012d", n);
  return id;
}

static BroJsonValue *
obj1 (const char *k, BroJsonValue *v)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, k, v);
  return o;
}

static char *
json_text (BroJsonValue *v) /* takes @v */
{
  char *t = v ? bro_json_value_to_text (v) : g_strdup ("null");
  if (v)
    bro_json_value_unref (v);
  return t;
}

static void
test_store_round_trips (void)
{
  g_autoptr (BroSqliteStore) store = new_store ("roundtrip");
  BroStore *s = BRO_STORE (store);
  g_autoptr (BroBroError) error = NULL;
  BroInstant t1 = at ("2026-10-01T12:00:00.000Z"), t2 = at ("2026-10-02T12:00:00.123Z");
  sqlite3 *db = raw_db ("roundtrip");

  /* a job: every JSON column */
  {
    g_autoptr (BroJobRecord) job = bro_job_record_new ();
    g_autoptr (BroJobRecord) back = NULL;
    BroStepKind steps[] = { BRO_STEP_KIND_AWAIT_MEDIA, BRO_STEP_KIND_PROBE };
    job->id = u (1);
    job->kind = BRO_JOB_KIND_VIDEO_DISC;
    job->state = BRO_JOB_STATE_RUNNING;
    job->has_outcome = TRUE;
    job->outcome = BRO_OUTCOME_FAILED;
    job->error = bro_bro_error_new ("process.couldNotStart", obj1 ("tool", bro_json_value_new_string ("makemkvcon")),
                                    bro_bro_error_new ("fs.notFound", obj1 ("path", bro_json_value_new_string ("/x")), NULL));
    job->blocked_by = obj1 ("drive", bro_json_value_new_string ("d1"));
    job->queue = BRO_QUEUE_ACQUISITION;
    job->position = 2;
    job->drive_id = g_strdup ("d1");
    job->has_media_generation = TRUE;
    job->media_generation = 3;
    job->automatic = TRUE;
    job->mode = g_strdup ("mkv");
    g_free (job->title);
    job->title = g_strdup ("Disc");
    job->fingerprint = g_strdup ("v1:aa");
    job->request = bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, "Disc");
    job->request->automatic = TRUE;
    job->request->drive_id = g_strdup ("d1");
    job->request->has_media_generation = TRUE;
    job->request->media_generation = 3;
    job->request->session_id = g_new0 (BroId, 1);
    *job->request->session_id = u (9);
    job->request->options = obj1 ("x", bro_json_value_new_integer (1));
    job->plan = bro_job_plan_new ();
    g_array_append_vals (job->plan->steps, steps, 2);
    job->plan->mode = g_strdup ("mkv");
    job->plan->profile_id = g_strdup ("default");
    job->plan->library_id = g_strdup ("lib");
    job->created_at = t1;
    job->has_started_at = TRUE;
    job->started_at = t2;
    g_assert_true (bro_job_repository_insert (bro_store_jobs (s), job, &error));
    back = bro_job_repository_load (bro_store_jobs (s), u (1), &error);
    g_assert_nonnull (back);
    g_assert_cmpstr (back->id.value, ==, job->id.value);
    g_assert_true (back->has_outcome && back->outcome == BRO_OUTCOME_FAILED);
    g_assert_cmpstr (back->error->code, ==, "process.couldNotStart");
    g_assert_cmpstr (back->error->cause->code, ==, "fs.notFound");
    g_assert_true (bro_json_value_equal (back->error->params, job->error->params));
    g_assert_true (bro_json_value_equal (back->blocked_by, job->blocked_by));
    g_assert_null (back->decision);
    g_assert_true (back->has_media_generation && back->media_generation == 3 && back->automatic);
    g_assert_cmpstr (back->request->title, ==, "Disc");
    g_assert_cmpstr (back->request->session_id->value, ==, u (9).value);
    g_assert_null (back->request->parent_id);
    g_assert_true (bro_json_value_equal (back->request->options, job->request->options));
    g_assert_cmpuint (back->plan->steps->len, ==, 2);
    g_assert_cmpint (g_array_index (back->plan->steps, BroStepKind, 1), ==, BRO_STEP_KIND_PROBE);
    g_assert_cmpstr (back->plan->library_id, ==, "lib");
    g_assert_true (back->has_started_at && back->started_at.unix_milliseconds == t2.unix_milliseconds && !back->has_finished_at);
  }

  /* a step, saved twice */
  {
    g_autoptr (BroStepRecord) step = bro_step_record_new ();
    g_autoptr (GPtrArray) back = NULL;
    BroStepRecord *b;
    step->job_id = u (1);
    step->kind = BRO_STEP_KIND_PROBE;
    step->state = BRO_STEP_STATE_SUCCEEDED;
    step->attempt = 2;
    step->output = bro_step_output_new (BRO_STEP_KIND_PROBE, obj1 ("titles", bro_json_value_new_integer (3)),
                                        bro_bro_message_new (BRO_MSG_JOB_CANCELLED, NULL, BRO_SEVERITY_WARNING));
    step->error = bro_bro_error_new ("job.cancelled", NULL, NULL);
    step->checkpoint = obj1 ("sector", bro_json_value_new_integer (10));
    g_assert_true (bro_step_repository_save (bro_store_steps (s), step, &error));
    step->attempt = 3;
    g_assert_true (bro_step_repository_save (bro_store_steps (s), step, &error));
    back = bro_step_repository_load (bro_store_steps (s), u (1), &error);
    g_assert_cmpuint (back->len, ==, 1);
    b = back->pdata[0];
    g_assert_cmpint (b->attempt, ==, 3);
    g_assert_true (bro_json_value_equal (b->output->data, step->output->data));
    g_assert_cmpint (b->output->summary->severity, ==, BRO_SEVERITY_WARNING);
    g_assert_cmpint (b->output->summary->code, ==, BRO_MSG_JOB_CANCELLED);
    g_assert_cmpstr (b->error->code, ==, "job.cancelled");
    g_assert_true (bro_json_value_equal (b->checkpoint, step->checkpoint));
  }

  /* a unit through the commit protocol */
  sqlite3_exec (db, "INSERT INTO libraries (id, name, path, marker_id, attached_at) VALUES ('lib', 'L', '/archive', 'm', '2026-10-01T12:00:00.000Z')",
                NULL, NULL, NULL);
  {
    g_autoptr (BroUnitRecord) unit = bro_unit_record_new ();
    g_autoptr (BroCommitIntent) intent = bro_commit_intent_new ();
    g_autoptr (GPtrArray) open = NULL;
    g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
    g_autoptr (BroUnitRecord) back = NULL;
    BroCommitItem *item;
    BroUnitFile *f;
    unit->id = u (100);
    unit->library_id = g_strdup ("lib");
    unit->has_job_id = TRUE;
    unit->job_id = u (1);
    unit->path = g_strdup ("Movies/Disc");
    unit->record_file = g_strdup ("bromelia-00000000.json");
    unit->record_version = 3;
    unit->state = BRO_UNIT_STATE_COMMITTING;
    unit->status = BRO_UNIT_STATUS_SUCCESS;
    unit->name = g_strdup ("Disc");
    unit->kind = BRO_MEDIA_KIND_MOVIE;
    unit->format = g_strdup ("bluray");
    unit->format_code = g_strdup ("BD");
    unit->encrypted = TRUE;
    unit->fingerprint = g_strdup ("v1:aa");
    unit->label = g_strdup ("DISC");
    unit->has_disc = TRUE;
    unit->disc = 1;
    unit->makemkv_version = g_strdup ("v2.0");
    unit->bytes = 1234;
    unit->file_count = 2;
    unit->attempts = 1;
    unit->created_at = t1;
    intent->unit_id = u (100);
    intent->job_id = u (1);
    intent->staging = g_strdup ("/staging");
    intent->destination = g_strdup ("/archive/Movies/Disc");
    intent->created_at = t1;
    item = bro_commit_item_new ();
    item->seq = 0;
    item->from_path = g_strdup ("a.mkv");
    item->to_path = g_strdup ("a.mkv");
    item->sha256 = g_strnfill (64, 'a');
    g_ptr_array_add (intent->items, item);
    item = bro_commit_item_new ();
    item->seq = 1;
    item->from_path = g_strdup ("extras");
    item->to_path = g_strdup ("extras");
    item->is_dir = TRUE;
    g_ptr_array_add (intent->items, item);
    g_assert_true (bro_unit_repository_begin_commit (bro_store_units (s), unit, intent, &error));
    g_assert_true (bro_unit_repository_mark_moved (bro_store_units (s), u (100), 1, &error));
    open = bro_unit_repository_open_intents (bro_store_units (s), &error);
    g_assert_cmpuint (open->len, ==, 1);
    g_assert_false (((BroCommitItem *) ((BroCommitIntent *) open->pdata[0])->items->pdata[0])->moved);
    g_assert_true (((BroCommitItem *) ((BroCommitIntent *) open->pdata[0])->items->pdata[1])->moved);
    unit->state = BRO_UNIT_STATE_COMMITTED;
    unit->has_committed_at = TRUE;
    unit->committed_at = t2;
    f = bro_unit_file_new ();
    f->path = g_strdup ("a.mkv");
    f->size = 1;
    f->sha256 = g_strnfill (64, 'a');
    f->role = g_strdup ("title");
    f->has_title = TRUE;
    g_ptr_array_add (files, f);
    g_assert_true (bro_unit_repository_finish_commit (bro_store_units (s), intent, unit, files, &error));
    g_clear_pointer (&open, g_ptr_array_unref);
    open = bro_unit_repository_open_intents (bro_store_units (s), &error);
    g_assert_cmpuint (open->len, ==, 0);
    back = bro_unit_repository_get (bro_store_units (s), u (100), &error);
    g_assert_cmpint (back->state, ==, BRO_UNIT_STATE_COMMITTED);
    g_assert_true (back->has_committed_at && back->committed_at.unix_milliseconds == t2.unix_milliseconds);
    g_assert_true (back->encrypted && back->has_disc && back->disc == 1 && !back->has_season);
    g_assert_cmpstr (back->fingerprint, ==, "v1:aa");
    g_assert_true (bro_unit_repository_mark_missing (bro_store_units (s), u (100), &error));
    g_clear_pointer (&back, bro_unit_record_free);
    back = bro_unit_repository_get (bro_store_units (s), u (100), &error);
    g_assert_cmpint (back->state, ==, BRO_UNIT_STATE_MISSING);
  }

  /* a check, a replica, a drive, the lookup cache, the outbox, kv */
  {
    g_autoptr (BroCheckRecord) check = bro_check_record_new ();
    g_autoptr (GPtrArray) checks = NULL;
    g_autoptr (BroReplicaRecord) replica = bro_replica_record_new ();
    g_autoptr (GPtrArray) replicas = NULL;
    g_autoptr (BroDriveRecord) drive = bro_drive_record_new ();
    g_autoptr (GPtrArray) drives = NULL;
    g_autoptr (BroHttpResponse) response = bro_http_response_new ();
    g_autoptr (BroHttpResponse) cached = NULL;
    g_autoptr (BroOutboxEntry) note = bro_outbox_entry_new ();
    g_autoptr (GPtrArray) due = NULL;
    g_autoptr (BroJsonValue) value = NULL;
    g_autoptr (BroBroError) http = bro_bro_error_new ("http.failed", NULL, NULL);
    g_autofree char *w1 = NULL, *w2 = NULL;
    const char *changed[] = { "a.mkv", NULL }, *missing[] = { "b.mkv", NULL };
    BroCheckRecord *c;
    check->id = u (200);
    check->has_unit_id = TRUE;
    check->unit_id = u (100);
    check->folder = g_strdup ("/archive/Movies/Disc");
    check->has_job_id = TRUE;
    check->job_id = u (1);
    check->started_at = t1;
    check->has_finished_at = TRUE;
    check->finished_at = t2;
    check->result = BRO_CHECK_RESULT_DAMAGED;
    check->files = 2;
    check->bytes = 1234;
    g_strfreev (check->changed);
    check->changed = g_strdupv ((char **) changed);
    g_strfreev (check->missing);
    check->missing = g_strdupv ((char **) missing);
    check->error = bro_bro_message_new (BRO_MSG_FS_FAILED, obj1 ("path", bro_json_value_new_string ("/a")), BRO_SEVERITY_ERROR);
    g_assert_true (bro_check_repository_insert (bro_store_checks (s), check, &error));
    checks = bro_check_repository_for_unit (bro_store_checks (s), u (100), &error);
    c = checks->pdata[0];
    g_assert_cmpstr (c->changed[0], ==, "a.mkv");
    g_assert_null (c->changed[1]);
    g_assert_cmpstr (c->missing[0], ==, "b.mkv");
    g_assert_null (c->unreadable[0]);
    g_assert_cmpint (c->error->code, ==, BRO_MSG_FS_FAILED);
    g_assert_cmpint (c->error->severity, ==, BRO_SEVERITY_ERROR);
    g_assert_cmpint (c->result, ==, BRO_CHECK_RESULT_DAMAGED);

    replica->id = u (300);
    replica->unit_id = u (100);
    replica->target_id = g_strdup ("nas");
    replica->path = g_strdup ("/nas/Disc");
    replica->state = BRO_REPLICA_STATE_COPYING;
    g_assert_true (bro_replica_repository_upsert (bro_store_replicas (s), replica, &error));
    replica->state = BRO_REPLICA_STATE_VERIFIED;
    replica->has_verified_at = TRUE;
    replica->verified_at = t2;
    g_assert_true (bro_replica_repository_upsert (bro_store_replicas (s), replica, &error));
    replicas = bro_replica_repository_for_unit (bro_store_replicas (s), u (100), &error);
    g_assert_cmpuint (replicas->len, ==, 1);
    g_assert_cmpint (((BroReplicaRecord *) replicas->pdata[0])->state, ==, BRO_REPLICA_STATE_VERIFIED);

    drive->id = g_strdup ("d1");
    drive->identification = g_strdup ("BD-RE X");
    drive->model = g_strdup ("X");
    drive->last_device = g_strdup ("/dev/rdisk4");
    drive->config_id = g_strdup ("left");
    drive->first_seen_at = drive->last_seen_at = t1;
    g_assert_true (bro_drive_repository_upsert (bro_store_drives (s), drive, &error));
    g_free (drive->last_device);
    drive->last_device = g_strdup ("/dev/rdisk5");
    drive->first_seen_at = drive->last_seen_at = t2;
    g_assert_true (bro_drive_repository_upsert (bro_store_drives (s), drive, &error));
    drives = bro_drive_repository_all (bro_store_drives (s), &error);
    g_assert_cmpstr (((BroDriveRecord *) drives->pdata[0])->last_device, ==, "/dev/rdisk5");
    g_assert_cmpint (((BroDriveRecord *) drives->pdata[0])->first_seen_at.unix_milliseconds, ==, t1.unix_milliseconds);

    response->status = 200;
    g_clear_pointer (&response->body, g_bytes_unref);
    response->body = g_bytes_new ("\x01\x02\x03", 3);
    g_assert_true (bro_lookup_cache_repository_put (bro_store_lookup_cache (s), "tmdb", "k", response, at ("2026-10-05T00:00:00.000Z"), &error));
    cached = bro_lookup_cache_repository_get (bro_store_lookup_cache (s), "tmdb", "k", &error);
    g_assert_true (g_bytes_equal (cached->body, response->body));

    note->id = u (400);
    note->target_id = g_strdup ("t");
    note->has_job_id = TRUE;
    note->job_id = u (1);
    note->title = bro_bro_message_new (BRO_MSG_NOTIFY_TEST_TITLE, NULL, BRO_SEVERITY_INFO);
    g_ptr_array_add (note->lines, bro_bro_message_new (BRO_MSG_NOTIFY_TEST_BODY, obj1 ("target", bro_json_value_new_string ("t")), BRO_SEVERITY_INFO));
    note->status = BRO_STATUS_WORD_SUCCESS;
    note->created_at = t1;
    note->has_next_attempt_at = TRUE;
    note->next_attempt_at = t1;
    g_assert_true (bro_outbox_repository_add (bro_store_outbox (s), note, &error));
    due = bro_outbox_repository_due (bro_store_outbox (s), t2, &error);
    g_assert_cmpuint (due->len, ==, 1);
    g_assert_cmpint (((BroOutboxEntry *) due->pdata[0])->title->code, ==, BRO_MSG_NOTIFY_TEST_TITLE);
    g_assert_cmpuint (((BroOutboxEntry *) due->pdata[0])->lines->len, ==, 1);
    g_assert_true (bro_outbox_repository_mark_failed (bro_store_outbox (s), u (400), http, NULL, &error));
    g_clear_pointer (&due, g_ptr_array_unref);
    due = bro_outbox_repository_due (bro_store_outbox (s), t2, &error);
    g_assert_cmpuint (due->len, ==, 0);
    g_assert_true (bro_outbox_repository_mark_sent (bro_store_outbox (s), u (400), &error));

    value = obj1 ("b", bro_json_value_new_bool (TRUE));
    g_assert_true (bro_key_value_repository_set (bro_store_kv (s), "a", value, &error));
    g_clear_pointer (&value, bro_json_value_unref);
    value = obj1 ("b", bro_json_value_new_bool (FALSE));
    g_assert_true (bro_key_value_repository_set (bro_store_kv (s), "a", value, &error));
    w1 = json_text (bro_key_value_repository_get (bro_store_kv (s), "a", &error));
    w2 = json_text (bro_json_value_ref (value));
    g_assert_cmpstr (w1, ==, w2);
    g_assert_null (bro_key_value_repository_get (bro_store_kv (s), "nope", &error));
  }
  g_assert_null (error);
  sqlite3_close (db);
}

static gboolean
set_two (BroStore *s, gpointer data, BroBroError **error)
{
  g_autoptr (BroJsonValue) a = bro_json_value_new_integer (1), b = bro_json_value_new_integer (2);
  return bro_key_value_repository_set (bro_store_kv (s), "a", a, error) && bro_key_value_repository_set (bro_store_kv (s), "b", b, error);
}

static gboolean
set_then_fail (BroStore *s, gpointer data, BroBroError **error)
{
  g_autoptr (BroJsonValue) a = bro_json_value_new_integer (10);
  bro_key_value_repository_set (bro_store_kv (s), "a", a, error);
  bro_bro_error_set (error, "job.cancelled", NULL);
  return FALSE;
}

static void
test_store_transaction (void)
{
  g_autoptr (BroSqliteStore) store = new_store ("tx");
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroJsonValue) a = NULL;
  g_autoptr (BroJsonValue) b = NULL;
  g_assert_true (bro_store_transaction (BRO_STORE (store), set_two, NULL, &error));
  g_assert_false (bro_store_transaction (BRO_STORE (store), set_then_fail, NULL, &error));
  g_assert_cmpstr (error->code, ==, "job.cancelled");
  a = bro_key_value_repository_get (bro_store_kv (BRO_STORE (store)), "a", NULL);
  b = bro_key_value_repository_get (bro_store_kv (BRO_STORE (store)), "b", NULL);
  g_assert_cmpint (a->integer, ==, 1);
  g_assert_cmpint (b->integer, ==, 2);
}

static void
dump_rows (sqlite3 *db, const char *sql, GPtrArray *out)
{
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2 (db, sql, -1, &stmt, NULL);
  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      int n = sqlite3_column_count (stmt);
      char **row = g_new0 (char *, n + 1);
      for (int i = 0; i < n; i++)
        row[i] = sqlite3_column_type (stmt, i) == SQLITE_NULL ? g_strdup ("\x01") : g_strdup ((const char *) sqlite3_column_text (stmt, i));
      g_ptr_array_add (out, row);
    }
  sqlite3_finalize (stmt);
}

/* The schema described as tools/check-contracts.py writes 0001_init.expected.txt. */
static char *
schema_dump (sqlite3 *db)
{
  g_autoptr (GString) s = g_string_new (NULL);
  g_autoptr (GPtrArray) tables = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  g_autoptr (GPtrArray) indexes = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  g_autoptr (GPtrArray) version = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  dump_rows (db, "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name", tables);
  for (guint t = 0; t < tables->len; t++)
    {
      const char *name = ((char **) tables->pdata[t])[0];
      g_autofree char *info = g_strdup_printf ("PRAGMA table_info(%s)", name);
      g_autofree char *fks = g_strdup_printf ("PRAGMA foreign_key_list(%s)", name);
      g_autoptr (GPtrArray) cols = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
      g_autoptr (GPtrArray) keys = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
      g_string_append_printf (s, "table %s\n", name);
      dump_rows (db, info, cols);
      for (guint c = 0; c < cols->len; c++)
        {
          char **col = cols->pdata[c];
          g_string_append_printf (s, "  %s %s%s", col[1], col[2], g_str_equal (col[3], "1") ? " NOT NULL" : "");
          if (!g_str_equal (col[4], "\x01"))
            g_string_append_printf (s, " DEFAULT %s", col[4]);
          if (!g_str_equal (col[5], "0"))
            g_string_append_printf (s, " PK%s", col[5]);
          g_string_append_c (s, '\n');
        }
      dump_rows (db, fks, keys);
      for (guint k = 0; k < keys->len; k++)
        {
          char **fk = keys->pdata[k];
          g_autofree char *on_delete = g_ascii_strdown (fk[6], -1);
          g_string_append_printf (s, "  fk %s -> %s(%s) on delete %s\n", fk[3], fk[2], fk[4], on_delete);
        }
    }
  dump_rows (db, "SELECT name, tbl_name, sql FROM sqlite_schema WHERE type = 'index' AND sql IS NOT NULL ORDER BY name", indexes);
  for (guint i = 0; i < indexes->len; i++)
    {
      char **ix = indexes->pdata[i];
      g_auto (GStrv) words = g_strsplit_set (ix[2], " \n\t", -1);
      g_autoptr (GString) joined = g_string_new (NULL);
      for (guint w = 0; words[w]; w++)
        if (*words[w])
          g_string_append_printf (joined, "%s%s", joined->len ? " " : "", words[w]);
      g_string_append_printf (s, "index %s on %s: %s\n", ix[0], ix[1], joined->str);
    }
  dump_rows (db, "PRAGMA user_version", version);
  g_string_append_printf (s, "user_version %s\n", ((char **) version->pdata[0])[0]);
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static void
test_store_schema (void)
{
  g_autoptr (BroSqliteStore) store = new_store ("schema");
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroUnitRecord) orphan = bro_unit_record_new ();
  g_autoptr (BroCommitIntent) intent = bro_commit_intent_new ();
  g_autofree char *expected = bro_test_fixture_text ("../schema/db/0001_init.expected.txt", NULL);
  g_autofree char *dump = NULL;
  g_autoptr (BroUnitRecord) none = NULL;
  g_autoptr (GPtrArray) mode = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  sqlite3 *db;
  g_assert_true (bro_sqlite_store_migrate (store, &error));
  db = raw_db ("schema");
  dump_rows (db, "PRAGMA journal_mode", mode);
  g_assert_cmpstr (((char **) mode->pdata[0])[0], ==, "wal");
  dump = schema_dump (db);
  g_assert_cmpstr (dump, ==, expected);
  sqlite3_close (db);
  /* A unit in a library the database doesn't have breaks a foreign key: store.failed, and nothing is written. */
  orphan->id = u (100);
  orphan->library_id = g_strdup ("nope");
  orphan->path = g_strdup ("p");
  orphan->record_file = g_strdup ("r");
  orphan->record_version = 3;
  orphan->name = g_strdup ("n");
  orphan->format = g_strdup ("dvd");
  orphan->format_code = g_strdup ("DVD");
  orphan->label = g_strdup ("");
  orphan->makemkv_version = g_strdup ("");
  orphan->attempts = 1;
  intent->unit_id = u (100);
  intent->job_id = u (1);
  intent->staging = g_strdup ("/s");
  intent->destination = g_strdup ("/d");
  g_assert_false (bro_unit_repository_begin_commit (bro_store_units (BRO_STORE (store)), orphan, intent, &error));
  g_assert_cmpstr (error->code, ==, "store.failed");
  none = bro_unit_repository_get (bro_store_units (BRO_STORE (store)), u (100), NULL);
  g_assert_null (none);
}
