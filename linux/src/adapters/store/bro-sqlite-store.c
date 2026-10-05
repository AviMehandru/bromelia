/* bro-sqlite-store.c */
#include "bro-sqlite-store.h"

#include "bro-episode-continuation.h"
#include "bro-label-parser.h"
#include "bro-message-code.h"
#include "bro-migrations.h"
#include <sqlite3.h>
#include <string.h>

/* ---- the connection, shared by the store and its transaction views ---- */

typedef struct {
  gatomicrefcount refs;
  sqlite3 *db;
  GMutex lock;
  BroClock *clock;
} Shared;

static Shared *
shared_ref (Shared *s)
{
  g_atomic_ref_count_inc (&s->refs);
  return s;
}

static void
shared_unref (Shared *s)
{
  if (!g_atomic_ref_count_dec (&s->refs))
    return;
  if (s->db)
    sqlite3_close_v2 (s->db);
  g_mutex_clear (&s->lock);
  g_clear_object (&s->clock);
  g_free (s);
}

struct _BroSqliteStore {
  GObject parent_instance;
  Shared *shared;
  gboolean in_transaction; /* a view inside bro_sqlite_store_transaction: the lock is held already */
};

static void store_iface_init (BroStoreInterface *iface);
static void jobs_iface_init (BroJobRepositoryInterface *iface);
static void steps_iface_init (BroStepRepositoryInterface *iface);
static void units_iface_init (BroUnitRepositoryInterface *iface);
static void catalog_iface_init (BroCatalogRepositoryInterface *iface);
static void checks_iface_init (BroCheckRepositoryInterface *iface);
static void replicas_iface_init (BroReplicaRepositoryInterface *iface);
static void drives_iface_init (BroDriveRepositoryInterface *iface);
static void lookup_iface_init (BroLookupCacheRepositoryInterface *iface);
static void outbox_iface_init (BroOutboxRepositoryInterface *iface);
static void kv_iface_init (BroKeyValueRepositoryInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroSqliteStore, bro_sqlite_store, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_STORE, store_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_JOB_REPOSITORY, jobs_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP_REPOSITORY, steps_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_UNIT_REPOSITORY, units_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CATALOG_REPOSITORY, catalog_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CHECK_REPOSITORY, checks_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_REPLICA_REPOSITORY, replicas_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_REPOSITORY, drives_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_LOOKUP_CACHE_REPOSITORY, lookup_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_OUTBOX_REPOSITORY, outbox_iface_init)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_KEY_VALUE_REPOSITORY, kv_iface_init))

#define STORE(x) BRO_SQLITE_STORE (x)

static gboolean
store_failed (BroBroError **error, const char *operation, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "operation", bro_json_value_new_string (operation));
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_STORE_FAILED), params);
  return FALSE;
}

static gboolean
sql_failed (BroSqliteStore *self, BroBroError **error)
{
  return store_failed (error, "query", self->shared->db ? sqlite3_errmsg (self->shared->db) : "the database is closed");
}

/* The connection whose transaction this thread is inside: the outer store would relock its lock (undefined
 * behaviour for a GMutex, a hang at best). */
static GPrivate in_transaction_of = G_PRIVATE_INIT (NULL);

/* store.reentered when this thread is inside one of the connection's transactions and @self isn't its view. */
static gboolean
check_not_reentered (BroSqliteStore *self, BroBroError **error)
{
  if (self->in_transaction || g_private_get (&in_transaction_of) != self->shared)
    return TRUE;
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_STORE_REENTERED), NULL);
  return FALSE;
}

/* Every call takes the lock unless it runs inside a transaction view; FALSE and @error set when the outer store is
 * used inside a transaction. */
static gboolean
enter (BroSqliteStore *self, BroBroError **error)
{
  if (!check_not_reentered (self, error))
    return FALSE;
  if (!self->in_transaction)
    g_mutex_lock (&self->shared->lock);
  return TRUE;
}

static void
leave (BroSqliteStore *self)
{
  if (!self->in_transaction)
    g_mutex_unlock (&self->shared->lock);
}

static gboolean
exec_sql (BroSqliteStore *self, const char *sql, BroBroError **error)
{
  char *message = NULL;
  if (!self->shared->db)
    return sql_failed (self, error);
  if (sqlite3_exec (self->shared->db, sql, NULL, NULL, &message) != SQLITE_OK)
    {
      store_failed (error, "query", message ? message : sqlite3_errmsg (self->shared->db));
      sqlite3_free (message);
      return FALSE;
    }
  return TRUE;
}

/* ---- arguments ---- */

typedef enum { ARG_NULL, ARG_INT, ARG_DOUBLE, ARG_TEXT, ARG_OWNED_TEXT, ARG_BLOB } ArgKind;

typedef struct {
  ArgKind kind;
  gint64 i;
  double d;
  const char *s;
  char *owned;
  gconstpointer blob;
  gsize size;
} Arg;

static Arg a_null (void) { return (Arg) { ARG_NULL }; }
static Arg a_int (gint64 v) { return (Arg) { ARG_INT, .i = v }; }
static Arg a_opt_int (gboolean has, gint64 v) { return has ? a_int (v) : a_null (); }
static Arg a_double (double v) { return (Arg) { ARG_DOUBLE, .d = v }; }
static Arg a_text (const char *s) { return s ? (Arg) { ARG_TEXT, .s = s } : a_null (); }
static Arg a_owned (char *s) { return s ? (Arg) { ARG_OWNED_TEXT, .owned = s } : a_null (); }
static Arg a_blob (gconstpointer p, gsize n) { return (Arg) { ARG_BLOB, .blob = p, .size = n }; }
static Arg a_bool (gboolean b) { return a_int (b ? 1 : 0); }
static Arg a_id (const BroId *id) { return a_text (id ? id->value : NULL); }
static Arg a_opt_id (gboolean has, const BroId *id) { return has ? a_id (id) : a_null (); }
static Arg a_time (BroInstant t) { return a_owned (bro_instant_format (t)); }
static Arg a_opt_time (gboolean has, BroInstant t) { return has ? a_time (t) : a_null (); }
static Arg a_json (BroJsonValue *v) /* takes @v */
{
  char *text;
  if (!v)
    return a_null ();
  text = bro_json_value_to_text (v);
  bro_json_value_unref (v);
  return a_owned (text);
}

static void
free_args (Arg *args, int n)
{
  for (int i = 0; i < n; i++)
    g_free (args[i].owned);
}

static sqlite3_stmt *
prepare (BroSqliteStore *self, const char *sql, Arg *args, int n, BroBroError **error)
{
  sqlite3_stmt *stmt = NULL;
  if (!self->shared->db || sqlite3_prepare_v2 (self->shared->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
      sql_failed (self, error);
      free_args (args, n);
      return NULL;
    }
  for (int i = 0; i < n; i++)
    switch (args[i].kind)
      {
      case ARG_NULL: sqlite3_bind_null (stmt, i + 1); break;
      case ARG_INT: sqlite3_bind_int64 (stmt, i + 1, args[i].i); break;
      case ARG_DOUBLE: sqlite3_bind_double (stmt, i + 1, args[i].d); break;
      case ARG_TEXT: sqlite3_bind_text (stmt, i + 1, args[i].s, -1, SQLITE_TRANSIENT); break;
      case ARG_OWNED_TEXT: sqlite3_bind_text (stmt, i + 1, args[i].owned, -1, SQLITE_TRANSIENT); break;
      case ARG_BLOB: sqlite3_bind_blob (stmt, i + 1, args[i].blob, (int) args[i].size, SQLITE_TRANSIENT); break;
      }
  free_args (args, n);
  return stmt;
}

/* Runs a statement that returns no rows (the caller holds the lock). */
static gboolean
execute (BroSqliteStore *self, const char *sql, Arg *args, int n, BroBroError **error)
{
  sqlite3_stmt *stmt = prepare (self, sql, args, n, error);
  int rc;
  if (!stmt)
    return FALSE;
  rc = sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW)
    return sql_failed (self, error);
  return TRUE;
}

#define N(args) ((int) G_N_ELEMENTS (args))

typedef gpointer (*MapFunc) (BroSqliteStore *self, sqlite3_stmt *row);

/* Every row, mapped (the caller holds the lock). NULL and @error set on failure. */
static GPtrArray *
query_rows (BroSqliteStore *self, const char *sql, Arg *args, int n, MapFunc map, GDestroyNotify free_func, BroBroError **error)
{
  sqlite3_stmt *stmt = prepare (self, sql, args, n, error);
  GPtrArray *out;
  int rc;
  if (!stmt)
    return NULL;
  out = g_ptr_array_new_with_free_func (free_func);
  while ((rc = sqlite3_step (stmt)) == SQLITE_ROW)
    g_ptr_array_add (out, map (self, stmt));
  sqlite3_finalize (stmt);
  if (rc != SQLITE_DONE)
    {
      g_ptr_array_unref (out);
      sql_failed (self, error);
      return NULL;
    }
  return out;
}

/* query_rows with the lock taken. */
static GPtrArray *
locked_rows (BroSqliteStore *self, const char *sql, Arg *args, int n, MapFunc map, GDestroyNotify free_func, BroBroError **error)
{
  GPtrArray *out;
  if (!enter (self, error))
    return NULL;
  out = query_rows (self, sql, args, n, map, free_func, error);
  leave (self);
  return out;
}

static gboolean
locked_execute (BroSqliteStore *self, const char *sql, Arg *args, int n, BroBroError **error)
{
  gboolean ok;
  if (!enter (self, error))
    return FALSE;
  ok = execute (self, sql, args, n, error);
  leave (self);
  return ok;
}

/* The first row of a query, or NULL (none, or a failure with @error set). */
static gpointer
first_row (GPtrArray *rows)
{
  gpointer first = NULL;
  if (rows && rows->len > 0)
    first = g_ptr_array_steal_index (rows, 0);
  if (rows)
    g_ptr_array_unref (rows);
  return first;
}

/* ---- reading columns ---- */

static gboolean col_null (sqlite3_stmt *r, int i) { return sqlite3_column_type (r, i) == SQLITE_NULL; }
static char *col_text (sqlite3_stmt *r, int i) { return g_strdup ((const char *) sqlite3_column_text (r, i)); }
static char *col_text_or_null (sqlite3_stmt *r, int i) { return col_null (r, i) ? NULL : col_text (r, i); }
static gint64 col_int (sqlite3_stmt *r, int i) { return sqlite3_column_int64 (r, i); }

static BroId
col_id (sqlite3_stmt *r, int i)
{
  BroId id = { { 0 } };
  const char *t = (const char *) sqlite3_column_text (r, i);
  if (t)
    g_strlcpy (id.value, t, sizeof id.value);
  return id;
}

static BroInstant
col_time (sqlite3_stmt *r, int i)
{
  BroInstant t = { 0 };
  bro_instant_parse ((const char *) sqlite3_column_text (r, i), &t);
  return t;
}

static BroJsonValue *
col_json (sqlite3_stmt *r, int i)
{
  return col_null (r, i) ? NULL : bro_json_value_parse ((const char *) sqlite3_column_text (r, i), -1);
}

#define OPT_INT(has, field, r, i) do { (has) = !col_null (r, i); (field) = (has) ? (int) col_int (r, i) : 0; } while (0)
#define OPT_ID(has, field, r, i) do { (has) = !col_null (r, i); if (has) (field) = col_id (r, i); } while (0)
#define OPT_TIME(has, field, r, i) do { (has) = !col_null (r, i); if (has) (field) = col_time (r, i); } while (0)

/* ---- JSON forms ---- */

static BroJsonValue *
error_json (const BroBroError *e)
{
  BroJsonValue *v = bro_json_value_new_object ();
  bro_json_value_set (v, "code", bro_json_value_new_string (e->code));
  if (bro_json_value_length (e->params) > 0)
    bro_json_value_set (v, "params", bro_json_value_ref (e->params));
  if (e->cause)
    bro_json_value_set (v, "cause", error_json (e->cause));
  return v;
}

static BroBroError *
error_of (const BroJsonValue *v)
{
  BroJsonValue *params = bro_json_value_member (v, "params");
  BroJsonValue *cause = bro_json_value_member (v, "cause");
  return bro_bro_error_new (bro_json_value_get_string (bro_json_value_member (v, "code"), ""), params ? bro_json_value_ref (params) : NULL,
                            cause && cause->kind != BRO_JSON_VALUE_NULL ? error_of (cause) : NULL);
}

static BroJsonValue *
message_json (const BroBroMessage *m)
{
  BroJsonValue *v = bro_json_value_new_object ();
  bro_json_value_set (v, "code", bro_json_value_new_string (bro_message_code_wire (m->code)));
  if (bro_json_value_length (m->params) > 0)
    bro_json_value_set (v, "params", bro_json_value_ref (m->params));
  if (m->severity != BRO_SEVERITY_INFO)
    bro_json_value_set (v, "severity", bro_json_value_new_string (bro_severity_to_wire (m->severity)));
  return v;
}

static BroBroMessage *
message_of (const BroJsonValue *v)
{
  BroMessageCode code = BRO_MSG_INTERNAL_UNEXPECTED;
  BroSeverity severity = BRO_SEVERITY_INFO;
  BroJsonValue *params = bro_json_value_member (v, "params");
  bro_message_code_parse (bro_json_value_get_string (bro_json_value_member (v, "code"), ""), &code);
  bro_severity_from_wire (bro_json_value_get_string (bro_json_value_member (v, "severity"), "info"), &severity);
  return bro_bro_message_new (code, params ? bro_json_value_ref (params) : NULL, severity);
}

static void
set_text (BroJsonValue *v, const char *key, const char *text)
{
  if (text)
    bro_json_value_set (v, key, bro_json_value_new_string (text));
}

static void
set_json (BroJsonValue *v, const char *key, BroJsonValue *value)
{
  if (value)
    bro_json_value_set (v, key, bro_json_value_ref (value));
}

static BroJsonValue *
request_json (const BroJobRequest *r)
{
  BroJsonValue *v = bro_json_value_new_object ();
  bro_json_value_set (v, "kind", bro_json_value_new_string (bro_job_kind_to_wire (r->kind)));
  bro_json_value_set (v, "title", bro_json_value_new_string (r->title ? r->title : ""));
  bro_json_value_set (v, "automatic", bro_json_value_new_bool (r->automatic));
  set_text (v, "driveId", r->drive_id);
  if (r->has_media_generation)
    bro_json_value_set (v, "mediaGeneration", bro_json_value_new_integer (r->media_generation));
  set_text (v, "sessionId", r->session_id ? r->session_id->value : NULL);
  set_text (v, "parentId", r->parent_id ? r->parent_id->value : NULL);
  set_text (v, "unitId", r->unit_id ? r->unit_id->value : NULL);
  set_json (v, "options", r->options);
  set_json (v, "details", r->details);
  return v;
}

static BroId *
id_member (const BroJsonValue *v, const char *key)
{
  const char *t = bro_json_value_get_string (bro_json_value_member (v, key), NULL);
  BroId *id;
  if (!t)
    return NULL;
  id = g_new0 (BroId, 1);
  g_strlcpy (id->value, t, sizeof id->value);
  return id;
}

static BroJsonValue *
json_member (const BroJsonValue *v, const char *key)
{
  BroJsonValue *m = bro_json_value_member (v, key);
  return m ? bro_json_value_ref (m) : NULL;
}

static BroJobRequest *
request_of (const BroJsonValue *v)
{
  BroJobKind kind = BRO_JOB_KIND_VIDEO_DISC;
  BroJsonValue *generation = bro_json_value_member (v, "mediaGeneration");
  BroJobRequest *r;
  bro_job_kind_from_wire (bro_json_value_get_string (bro_json_value_member (v, "kind"), ""), &kind);
  r = bro_job_request_new (kind, bro_json_value_get_string (bro_json_value_member (v, "title"), ""));
  r->automatic = bro_json_value_get_bool (bro_json_value_member (v, "automatic"), FALSE);
  r->drive_id = g_strdup (bro_json_value_get_string (bro_json_value_member (v, "driveId"), NULL));
  r->has_media_generation = generation != NULL;
  r->media_generation = bro_json_value_get_integer (generation, 0);
  r->session_id = id_member (v, "sessionId");
  r->parent_id = id_member (v, "parentId");
  r->unit_id = id_member (v, "unitId");
  r->options = json_member (v, "options");
  r->details = json_member (v, "details");
  return r;
}

static BroJsonValue *
plan_json (const BroJobPlan *p)
{
  BroJsonValue *v = bro_json_value_new_object ();
  BroJsonValue *steps = bro_json_value_new_array ();
  for (guint i = 0; i < p->steps->len; i++)
    bro_json_value_append (steps, bro_json_value_new_string (bro_step_kind_to_wire (g_array_index (p->steps, BroStepKind, i))));
  bro_json_value_set (v, "steps", steps);
  set_text (v, "mode", p->mode);
  set_text (v, "profileId", p->profile_id);
  set_text (v, "libraryId", p->library_id);
  set_json (v, "details", p->details);
  return v;
}

static BroJobPlan *
plan_of (const BroJsonValue *v)
{
  BroJobPlan *p = bro_job_plan_new ();
  BroJsonValue *steps = bro_json_value_member (v, "steps");
  for (guint i = 0; steps && i < bro_json_value_length (steps); i++)
    {
      BroStepKind k;
      if (bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_at (steps, i), ""), &k))
        g_array_append_val (p->steps, k);
    }
  p->mode = g_strdup (bro_json_value_get_string (bro_json_value_member (v, "mode"), NULL));
  p->profile_id = g_strdup (bro_json_value_get_string (bro_json_value_member (v, "profileId"), NULL));
  p->library_id = g_strdup (bro_json_value_get_string (bro_json_value_member (v, "libraryId"), NULL));
  p->details = json_member (v, "details");
  return p;
}

static BroJsonValue *
output_json (const BroStepOutput *o)
{
  BroJsonValue *v = bro_json_value_new_object ();
  bro_json_value_set (v, "kind", bro_json_value_new_string (bro_step_kind_to_wire (o->kind)));
  bro_json_value_set (v, "data", o->data ? bro_json_value_ref (o->data) : bro_json_value_new_null ());
  if (o->summary)
    bro_json_value_set (v, "summary", message_json (o->summary));
  return v;
}

static BroStepOutput *
output_of (const BroJsonValue *v)
{
  BroStepKind kind = BRO_STEP_KIND_AWAIT_MEDIA;
  BroJsonValue *data = bro_json_value_member (v, "data");
  BroJsonValue *summary = bro_json_value_member (v, "summary");
  bro_step_kind_from_wire (bro_json_value_get_string (bro_json_value_member (v, "kind"), ""), &kind);
  return bro_step_output_new (kind, data ? bro_json_value_ref (data) : bro_json_value_new_null (),
                              summary && summary->kind != BRO_JSON_VALUE_NULL ? message_of (summary) : NULL);
}

static BroJsonValue *
strings_json (GStrv list)
{
  BroJsonValue *v = bro_json_value_new_array ();
  for (guint i = 0; list && list[i]; i++)
    bro_json_value_append (v, bro_json_value_new_string (list[i]));
  return v;
}

static GStrv
strings_of (BroJsonValue *v) /* takes @v */
{
  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; v && i < bro_json_value_length (v); i++)
    g_ptr_array_add (out, g_strdup (bro_json_value_get_string (bro_json_value_at (v, i), "")));
  g_ptr_array_add (out, NULL);
  if (v)
    bro_json_value_unref (v);
  return (GStrv) g_ptr_array_free (out, FALSE);
}

/* ---- jobs ---- */

#define JOB_COLUMNS "id, kind, parent_id, state, outcome, error, blocked_by, decision, queue, position, drive_id, media_generation, " \
                    "automatic, mode, title, fingerprint, request, plan, unit_id, created_at, started_at, finished_at"

static int
job_args (const BroJobRecord *j, Arg *a)
{
  int n = 0;
  a[n++] = a_id (&j->id);
  a[n++] = a_text (bro_job_kind_to_wire (j->kind));
  a[n++] = a_opt_id (j->has_parent_id, &j->parent_id);
  a[n++] = a_text (bro_job_state_to_wire (j->state));
  a[n++] = j->has_outcome ? a_text (bro_outcome_to_wire (j->outcome)) : a_null ();
  a[n++] = a_json (j->error ? error_json (j->error) : NULL);
  a[n++] = a_json (j->blocked_by ? bro_json_value_ref (j->blocked_by) : NULL);
  a[n++] = a_json (j->decision ? bro_json_value_ref (j->decision) : NULL);
  a[n++] = a_text (bro_queue_to_wire (j->queue));
  a[n++] = a_int (j->position);
  a[n++] = a_text (j->drive_id);
  a[n++] = a_opt_int (j->has_media_generation, j->media_generation);
  a[n++] = a_bool (j->automatic);
  a[n++] = a_text (j->mode);
  a[n++] = a_text (j->title ? j->title : "");
  a[n++] = a_text (j->fingerprint);
  a[n++] = a_json (request_json (j->request));
  a[n++] = a_json (j->plan ? plan_json (j->plan) : NULL);
  a[n++] = a_opt_id (j->has_unit_id, &j->unit_id);
  a[n++] = a_time (j->created_at);
  a[n++] = a_opt_time (j->has_started_at, j->started_at);
  a[n++] = a_opt_time (j->has_finished_at, j->finished_at);
  return n;
}

static gpointer
map_job (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroJobRecord *j = bro_job_record_new ();
  g_autoptr (BroJsonValue) error = col_json (r, 5);
  g_autoptr (BroJsonValue) request = col_json (r, 16);
  g_autoptr (BroJsonValue) plan = col_json (r, 17);
  j->id = col_id (r, 0);
  bro_job_kind_from_wire ((const char *) sqlite3_column_text (r, 1), &j->kind);
  OPT_ID (j->has_parent_id, j->parent_id, r, 2);
  bro_job_state_from_wire ((const char *) sqlite3_column_text (r, 3), &j->state);
  j->has_outcome = !col_null (r, 4) && bro_outcome_from_wire ((const char *) sqlite3_column_text (r, 4), &j->outcome);
  j->error = error ? error_of (error) : NULL;
  j->blocked_by = col_json (r, 6);
  j->decision = col_json (r, 7);
  bro_queue_from_wire ((const char *) sqlite3_column_text (r, 8), &j->queue);
  j->position = (int) col_int (r, 9);
  j->drive_id = col_text_or_null (r, 10);
  j->has_media_generation = !col_null (r, 11);
  j->media_generation = col_int (r, 11);
  j->automatic = col_int (r, 12) != 0;
  j->mode = col_text_or_null (r, 13);
  g_free (j->title);
  j->title = col_text (r, 14);
  j->fingerprint = col_text_or_null (r, 15);
  j->request = request ? request_of (request) : bro_job_request_new (BRO_JOB_KIND_VIDEO_DISC, "");
  j->plan = plan ? plan_of (plan) : NULL;
  OPT_ID (j->has_unit_id, j->unit_id, r, 18);
  j->created_at = col_time (r, 19);
  OPT_TIME (j->has_started_at, j->started_at, r, 20);
  OPT_TIME (j->has_finished_at, j->finished_at, r, 21);
  return j;
}

static gboolean
job_insert (BroJobRepository *repo, const BroJobRecord *job, BroBroError **error)
{
  Arg a[22];
  int n = job_args (job, a);
  return locked_execute (STORE (repo), "INSERT INTO jobs (" JOB_COLUMNS ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                         a, n, error);
}

static gboolean
job_update (BroJobRepository *repo, const BroJobRecord *job, BroBroError **error)
{
  Arg a[22], b[22];
  int n = job_args (job, a);
  /* id last */
  for (int i = 1; i < n; i++)
    b[i - 1] = a[i];
  b[n - 1] = a[0];
  return locked_execute (STORE (repo),
                         "UPDATE jobs SET kind = ?, parent_id = ?, state = ?, outcome = ?, error = ?, blocked_by = ?, decision = ?, queue = ?, "
                         "position = ?, drive_id = ?, media_generation = ?, automatic = ?, mode = ?, title = ?, fingerprint = ?, request = ?, plan = ?, "
                         "unit_id = ?, created_at = ?, started_at = ?, finished_at = ? WHERE id = ?",
                         b, n, error);
}

static BroJobRecord *
job_load (BroJobRepository *repo, BroId job_id, BroBroError **error)
{
  Arg a[] = { a_id (&job_id) };
  return first_row (locked_rows (STORE (repo), "SELECT " JOB_COLUMNS " FROM jobs WHERE id = ?", a, N (a), map_job,
                                 (GDestroyNotify) bro_job_record_free, error));
}

static GPtrArray *
job_active (BroJobRepository *repo, BroBroError **error)
{
  return locked_rows (STORE (repo),
                      "SELECT " JOB_COLUMNS " FROM jobs WHERE state <> 'finished' ORDER BY "
                      "CASE queue WHEN 'acquisition' THEN 0 WHEN 'processing' THEN 1 ELSE 2 END, position, created_at, id",
                      NULL, 0, map_job, (GDestroyNotify) bro_job_record_free, error);
}

static GPtrArray *
job_query (BroJobRepository *repo, const BroJobQuery *q, BroBroError **error)
{
  Arg a[] = { q->has_state ? a_text (bro_job_state_to_wire (q->state)) : a_null (), q->has_kind ? a_text (bro_job_kind_to_wire (q->kind)) : a_null (),
              a_opt_time (q->has_finished_after, q->finished_after), a_int (q->limit), a_int (q->offset) };
  return locked_rows (STORE (repo),
                      "SELECT " JOB_COLUMNS " FROM jobs WHERE (?1 IS NULL OR state = ?1) AND (?2 IS NULL OR kind = ?2) "
                      "AND (?3 IS NULL OR finished_at > ?3) ORDER BY created_at DESC, id DESC LIMIT ?4 OFFSET ?5",
                      a, N (a), map_job, (GDestroyNotify) bro_job_record_free, error);
}

static void
jobs_iface_init (BroJobRepositoryInterface *iface)
{
  iface->insert = job_insert;
  iface->update = job_update;
  iface->load = job_load;
  iface->active = job_active;
  iface->query = job_query;
}

/* ---- steps ---- */

#define STEP_COLUMNS "job_id, seq, kind, state, attempt, started_at, finished_at, output, error, checkpoint"

static gpointer
map_step (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroStepRecord *s = bro_step_record_new ();
  g_autoptr (BroJsonValue) output = col_json (r, 7);
  g_autoptr (BroJsonValue) error = col_json (r, 8);
  s->job_id = col_id (r, 0);
  s->seq = (int) col_int (r, 1);
  bro_step_kind_from_wire ((const char *) sqlite3_column_text (r, 2), &s->kind);
  bro_step_state_from_wire ((const char *) sqlite3_column_text (r, 3), &s->state);
  s->attempt = (int) col_int (r, 4);
  OPT_TIME (s->has_started_at, s->started_at, r, 5);
  OPT_TIME (s->has_finished_at, s->finished_at, r, 6);
  s->output = output ? output_of (output) : NULL;
  s->error = error ? error_of (error) : NULL;
  s->checkpoint = col_json (r, 9);
  return s;
}

static gboolean
step_save (BroStepRepository *repo, const BroStepRecord *s, BroBroError **error)
{
  Arg a[] = { a_id (&s->job_id), a_int (s->seq), a_text (bro_step_kind_to_wire (s->kind)), a_text (bro_step_state_to_wire (s->state)),
              a_int (s->attempt), a_opt_time (s->has_started_at, s->started_at), a_opt_time (s->has_finished_at, s->finished_at),
              a_json (s->output ? output_json (s->output) : NULL), a_json (s->error ? error_json (s->error) : NULL),
              a_json (s->checkpoint ? bro_json_value_ref (s->checkpoint) : NULL) };
  return locked_execute (STORE (repo), "INSERT OR REPLACE INTO job_steps (" STEP_COLUMNS ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", a, N (a), error);
}

static GPtrArray *
step_completed (BroStepRepository *repo, BroId job_id, BroBroError **error)
{
  Arg a[] = { a_id (&job_id) };
  return locked_rows (STORE (repo), "SELECT " STEP_COLUMNS " FROM job_steps WHERE job_id = ? AND state = 'succeeded' ORDER BY seq", a, N (a),
                      map_step, (GDestroyNotify) bro_step_record_free, error);
}

static GPtrArray *
step_load (BroStepRepository *repo, BroId job_id, BroBroError **error)
{
  Arg a[] = { a_id (&job_id) };
  return locked_rows (STORE (repo), "SELECT " STEP_COLUMNS " FROM job_steps WHERE job_id = ? ORDER BY seq", a, N (a), map_step,
                      (GDestroyNotify) bro_step_record_free, error);
}

static void
steps_iface_init (BroStepRepositoryInterface *iface)
{
  iface->save = step_save;
  iface->completed = step_completed;
  iface->load = step_load;
}

/* ---- units ---- */

#define UNIT_COLUMNS "u.id, u.library_id, u.physical_disc_id, u.job_id, u.path, u.record_file, u.record_version, u.state, u.status, u.name, " \
                     "u.kind, u.format, u.format_code, u.encrypted, u.fingerprint, u.label, u.season, u.part, u.volume, u.disc, u.makemkv_version, " \
                     "u.bytes, u.file_count, u.attempts, u.created_at, u.committed_at"

#define UNIT_UPSERT                                                                                                                               \
  "INSERT INTO archive_units (id, library_id, physical_disc_id, job_id, path, record_file, record_version, state, status, name, kind, format, "    \
  "format_code, encrypted, fingerprint, label, season, part, volume, disc, makemkv_version, bytes, file_count, attempts, created_at, committed_at) " \
  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "                                                         \
  "ON CONFLICT (id) DO UPDATE SET library_id = excluded.library_id, physical_disc_id = excluded.physical_disc_id, job_id = excluded.job_id, "      \
  "path = excluded.path, record_file = excluded.record_file, record_version = excluded.record_version, state = excluded.state, "                  \
  "status = excluded.status, name = excluded.name, kind = excluded.kind, format = excluded.format, format_code = excluded.format_code, "          \
  "encrypted = excluded.encrypted, fingerprint = excluded.fingerprint, label = excluded.label, season = excluded.season, part = excluded.part, "  \
  "volume = excluded.volume, disc = excluded.disc, makemkv_version = excluded.makemkv_version, bytes = excluded.bytes, "                          \
  "file_count = excluded.file_count, attempts = excluded.attempts, created_at = excluded.created_at, committed_at = excluded.committed_at"

static gboolean
unit_upsert (BroSqliteStore *self, const BroUnitRecord *u, BroBroError **error)
{
  Arg a[] = { a_id (&u->id), a_text (u->library_id), a_opt_id (u->has_physical_disc_id, &u->physical_disc_id), a_opt_id (u->has_job_id, &u->job_id),
              a_text (u->path), a_text (u->record_file), a_int (u->record_version), a_text (bro_unit_state_to_wire (u->state)),
              a_text (bro_unit_status_to_wire (u->status)), a_text (u->name), a_text (bro_media_kind_to_wire (u->kind)), a_text (u->format),
              a_text (u->format_code), a_bool (u->encrypted), a_text (u->fingerprint), a_text (u->label ? u->label : ""),
              a_opt_int (u->has_season, u->season), a_opt_int (u->has_part, u->part), a_opt_int (u->has_volume, u->volume),
              a_opt_int (u->has_disc, u->disc), a_text (u->makemkv_version ? u->makemkv_version : ""), a_int (u->bytes), a_int (u->file_count),
              a_int (u->attempts), a_time (u->created_at), a_opt_time (u->has_committed_at, u->committed_at) };
  return execute (self, UNIT_UPSERT, a, N (a), error);
}

static gpointer
map_unit (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroUnitRecord *u = bro_unit_record_new ();
  u->id = col_id (r, 0);
  u->library_id = col_text (r, 1);
  OPT_ID (u->has_physical_disc_id, u->physical_disc_id, r, 2);
  OPT_ID (u->has_job_id, u->job_id, r, 3);
  u->path = col_text (r, 4);
  u->record_file = col_text (r, 5);
  u->record_version = (int) col_int (r, 6);
  bro_unit_state_from_wire ((const char *) sqlite3_column_text (r, 7), &u->state);
  bro_unit_status_from_wire ((const char *) sqlite3_column_text (r, 8), &u->status);
  u->name = col_text (r, 9);
  bro_media_kind_from_wire ((const char *) sqlite3_column_text (r, 10), &u->kind);
  u->format = col_text (r, 11);
  u->format_code = col_text (r, 12);
  u->encrypted = col_int (r, 13) != 0;
  u->fingerprint = col_text_or_null (r, 14);
  u->label = col_text (r, 15);
  OPT_INT (u->has_season, u->season, r, 16);
  OPT_INT (u->has_part, u->part, r, 17);
  OPT_INT (u->has_volume, u->volume, r, 18);
  OPT_INT (u->has_disc, u->disc, r, 19);
  u->makemkv_version = col_text (r, 20);
  u->bytes = col_int (r, 21);
  u->file_count = (int) col_int (r, 22);
  u->attempts = (int) col_int (r, 23);
  u->created_at = col_time (r, 24);
  OPT_TIME (u->has_committed_at, u->committed_at, r, 25);
  return u;
}

/* Runs @body inside BEGIN IMMEDIATE … COMMIT unless already in a transaction; the lock is taken. */
typedef gboolean (*AtomicFunc) (BroSqliteStore *self, gconstpointer a, gconstpointer b, gconstpointer c, BroBroError **error);

static gboolean
atomic (BroSqliteStore *self, AtomicFunc body, gconstpointer a, gconstpointer b, gconstpointer c, BroBroError **error)
{
  gboolean ok;
  if (!enter (self, error))
    return FALSE;
  if (self->in_transaction)
    ok = body (self, a, b, c, error);
  else if ((ok = exec_sql (self, "BEGIN IMMEDIATE", error)))
    {
      ok = body (self, a, b, c, error);
      if (ok)
        ok = exec_sql (self, "COMMIT", error);
      if (!ok)
        exec_sql (self, "ROLLBACK", NULL);
    }
  leave (self);
  return ok;
}

static gboolean
begin_commit_body (BroSqliteStore *self, gconstpointer unit, gconstpointer intent_p, gconstpointer unused, BroBroError **error)
{
  const BroCommitIntent *intent = intent_p;
  if (!unit_upsert (self, unit, error))
    return FALSE;
  {
    /* Built only now: execute () frees the arguments, and an early return would leak them. */
    Arg a[] = { a_id (&intent->unit_id), a_id (&intent->job_id), a_text (intent->staging), a_text (intent->destination), a_bool (intent->merge),
                a_bool (intent->quarantine), a_time (intent->created_at) };
    if (!execute (self, "INSERT INTO commit_intents (unit_id, job_id, staging, destination, merge, quarantine, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                  a, N (a), error))
      return FALSE;
  }
  for (guint i = 0; intent->items && i < intent->items->len; i++)
    {
      const BroCommitItem *it = intent->items->pdata[i];
      Arg b[] = { a_id (&intent->unit_id), a_int (it->seq), a_text (it->from_path), a_text (it->to_path), a_text (it->sha256), a_bool (it->is_dir),
                  a_bool (it->moved) };
      if (!execute (self, "INSERT INTO commit_items (unit_id, seq, from_path, to_path, sha256, is_dir, moved) VALUES (?, ?, ?, ?, ?, ?, ?)", b, N (b),
                    error))
        return FALSE;
    }
  return TRUE;
}

static gboolean
unit_begin_commit (BroUnitRepository *repo, const BroUnitRecord *unit, const BroCommitIntent *intent, BroBroError **error)
{
  return atomic (STORE (repo), begin_commit_body, unit, intent, NULL, error);
}

static gboolean
unit_mark_moved (BroUnitRepository *repo, BroId unit_id, int seq, BroBroError **error)
{
  Arg a[] = { a_id (&unit_id), a_int (seq) };
  return locked_execute (STORE (repo), "UPDATE commit_items SET moved = 1 WHERE unit_id = ? AND seq = ?", a, N (a), error);
}

static gboolean
finish_commit_body (BroSqliteStore *self, gconstpointer intent_p, gconstpointer record_p, gconstpointer files_p, BroBroError **error)
{
  const BroCommitIntent *intent = intent_p;
  const BroUnitRecord *record = record_p;
  GPtrArray *files = (GPtrArray *) files_p;
  Arg del[] = { a_id (&record->id) };
  Arg gone[] = { a_id (&intent->unit_id) };
  if (!unit_upsert (self, record, error) || !execute (self, "DELETE FROM unit_files WHERE unit_id = ?", del, N (del), error))
    return FALSE;
  for (guint i = 0; files && i < files->len; i++)
    {
      const BroUnitFile *f = files->pdata[i];
      Arg a[] = { a_id (&record->id), a_text (f->path), a_int (f->size), a_text (f->sha256), a_text (f->role ? f->role : "title"),
                  a_opt_int (f->has_title, f->title), a_opt_int (f->has_episode, f->episode) };
      if (!execute (self, "INSERT INTO unit_files (unit_id, path, size, sha256, role, title, episode) VALUES (?, ?, ?, ?, ?, ?, ?)", a, N (a), error))
        return FALSE;
    }
  return execute (self, "DELETE FROM commit_intents WHERE unit_id = ?", gone, N (gone), error);
}

static gboolean
unit_finish_commit (BroUnitRepository *repo, const BroCommitIntent *intent, const BroUnitRecord *record, GPtrArray *files, BroBroError **error)
{
  return atomic (STORE (repo), finish_commit_body, intent, record, files, error);
}

static gpointer
map_intent (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroCommitIntent *i = bro_commit_intent_new ();
  i->unit_id = col_id (r, 0);
  i->job_id = col_id (r, 1);
  i->staging = col_text (r, 2);
  i->destination = col_text (r, 3);
  i->merge = col_int (r, 4) != 0;
  i->quarantine = col_int (r, 5) != 0;
  i->created_at = col_time (r, 6);
  return i;
}

static gpointer
map_item (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroCommitItem *it = bro_commit_item_new ();
  it->seq = (int) col_int (r, 0);
  it->from_path = col_text (r, 1);
  it->to_path = col_text (r, 2);
  it->sha256 = col_text_or_null (r, 3);
  it->is_dir = col_int (r, 4) != 0;
  it->moved = col_int (r, 5) != 0;
  return it;
}

static GPtrArray *
unit_open_intents (BroUnitRepository *repo, BroBroError **error)
{
  BroSqliteStore *self = STORE (repo);
  GPtrArray *intents;
  if (!enter (self, error))
    return NULL;
  intents = query_rows (self, "SELECT unit_id, job_id, staging, destination, merge, quarantine, created_at FROM commit_intents ORDER BY created_at, unit_id",
                        NULL, 0, map_intent, (GDestroyNotify) bro_commit_intent_free, error);
  for (guint i = 0; intents && i < intents->len; i++)
    {
      BroCommitIntent *intent = intents->pdata[i];
      Arg a[] = { a_id (&intent->unit_id) };
      GPtrArray *items = query_rows (self, "SELECT seq, from_path, to_path, sha256, is_dir, moved FROM commit_items WHERE unit_id = ? ORDER BY seq",
                                     a, N (a), map_item, (GDestroyNotify) bro_commit_item_free, error);
      if (!items)
        {
          g_clear_pointer (&intents, g_ptr_array_unref);
          break;
        }
      while (items->len)
        g_ptr_array_add (intent->items, g_ptr_array_steal_index (items, 0));
      g_ptr_array_unref (items);
    }
  leave (self);
  return intents;
}

static BroUnitRecord *
unit_get (BroUnitRepository *repo, BroId unit_id, BroBroError **error)
{
  Arg a[] = { a_id (&unit_id) };
  return first_row (locked_rows (STORE (repo), "SELECT " UNIT_COLUMNS " FROM archive_units u WHERE u.id = ?", a, N (a), map_unit,
                                 (GDestroyNotify) bro_unit_record_free, error));
}

static GPtrArray *
unit_query (BroUnitRepository *repo, const BroUnitQuery *q, BroBroError **error)
{
  Arg a[] = { a_text (q->library_id), a_text (q->text), q->has_state ? a_text (bro_unit_state_to_wire (q->state)) : a_null (), a_int (q->limit),
              a_int (q->offset) };
  return locked_rows (STORE (repo),
                      "SELECT " UNIT_COLUMNS " FROM archive_units u WHERE (?1 IS NULL OR u.library_id = ?1) "
                      "AND (?2 IS NULL OR instr(lower(u.name), lower(?2)) > 0) AND (?3 IS NULL OR u.state = ?3) "
                      "ORDER BY u.created_at DESC, u.id DESC LIMIT ?4 OFFSET ?5",
                      a, N (a), map_unit, (GDestroyNotify) bro_unit_record_free, error);
}

static GPtrArray *
unit_least_recently_verified (BroUnitRepository *repo, int limit, BroBroError **error)
{
  Arg a[] = { a_int (limit) };
  return locked_rows (STORE (repo),
                      "SELECT " UNIT_COLUMNS " FROM archive_units u WHERE u.state = 'committed' "
                      "ORDER BY (SELECT max(c.finished_at) FROM checks c WHERE c.unit_id = u.id) IS NOT NULL, "
                      "(SELECT max(c.finished_at) FROM checks c WHERE c.unit_id = u.id), u.created_at, u.id LIMIT ?",
                      a, N (a), map_unit, (GDestroyNotify) bro_unit_record_free, error);
}

static gboolean
unit_mark_missing (BroUnitRepository *repo, BroId unit_id, BroBroError **error)
{
  Arg a[] = { a_id (&unit_id) };
  return locked_execute (STORE (repo), "UPDATE archive_units SET state = 'missing' WHERE id = ?", a, N (a), error);
}

static void
units_iface_init (BroUnitRepositoryInterface *iface)
{
  iface->begin_commit = unit_begin_commit;
  iface->mark_moved = unit_mark_moved;
  iface->finish_commit = unit_finish_commit;
  iface->open_intents = unit_open_intents;
  iface->get = unit_get;
  iface->query = unit_query;
  iface->least_recently_verified = unit_least_recently_verified;
  iface->mark_missing = unit_mark_missing;
}

/* ---- catalogue ---- */

static GPtrArray *
catalog_archived_before (BroCatalogRepository *repo, const char *fingerprint, BroBroError **error)
{
  Arg a[] = { a_text (fingerprint) };
  return locked_rows (STORE (repo),
                      "SELECT " UNIT_COLUMNS " FROM archive_units u WHERE u.fingerprint = ? AND u.state = 'committed' ORDER BY u.committed_at DESC, u.id",
                      a, N (a), map_unit, (GDestroyNotify) bro_unit_record_free, error);
}

static gpointer
map_candidate (BroSqliteStore *self, sqlite3_stmt *r)
{
  g_autoptr (BroLabel) label = bro_label_parser_parse ((const char *) sqlite3_column_text (r, 1));
  g_autofree char *library = col_text (r, 6);
  g_autofree char *folder = NULL;
  BroArchivedDisc *d;
  size_t n = strlen (library);
  while (n > 0 && library[n - 1] == '/')
    library[--n] = '\0';
  folder = g_strconcat (library, "/", (const char *) sqlite3_column_text (r, 7), NULL);
  d = bro_archived_disc_new ((const char *) sqlite3_column_text (r, 0), label->title, folder);
  d->season = col_null (r, 2) ? -1 : (int) col_int (r, 2);
  d->part = col_null (r, 3) ? -1 : (int) col_int (r, 3);
  d->volume = col_null (r, 4) ? -1 : (int) col_int (r, 4);
  d->disc = col_null (r, 5) ? -1 : (int) col_int (r, 5);
  d->last_episode = col_null (r, 8) ? -1 : (int) col_int (r, 8);
  return d;
}

/* Committed TV discs (success or errors), as EpisodeContinuation sees them; only disc @disc when it is 0 or more. */
static GPtrArray *
candidates (BroSqliteStore *self, int disc, BroBroError **error)
{
  Arg a[] = { disc >= 0 ? a_int (disc) : a_null () };
  return locked_rows (self,
                      "SELECT u.name, u.label, u.season, u.part, u.volume, u.disc, l.path, u.path, "
                      "(SELECT max(f.episode) FROM unit_files f WHERE f.unit_id = u.id) FROM archive_units u JOIN libraries l ON l.id = u.library_id "
                      "WHERE u.kind = 'tv' AND u.state = 'committed' AND u.status IN ('success', 'errors') AND (?1 IS NULL OR u.disc = ?1) "
                      "ORDER BY u.committed_at, u.id",
                      a, N (a), map_candidate, (GDestroyNotify) bro_archived_disc_free, error);
}

static BroArchivedDisc *
catalog_previous_disc (BroCatalogRepository *repo, const BroContinuationQuery *query, BroBroError **error)
{
  g_autoptr (GPtrArray) all = NULL;
  BroArchivedDisc *best = NULL;
  if (query->disc <= 1)
    return NULL;
  all = candidates (STORE (repo), query->disc - 1, error);
  for (guint i = 0; all && i < all->len; i++)
    {
      BroArchivedDisc *c = all->pdata[i];
      if (bro_episode_continuation_same_set (c, query) && c->last_episode >= 0 && (!best || c->last_episode > best->last_episode))
        best = c;
    }
  if (!best)
    return NULL;
  {
    BroArchivedDisc *copy = bro_archived_disc_new (best->name, best->label_title, best->folder);
    copy->season = best->season;
    copy->part = best->part;
    copy->volume = best->volume;
    copy->disc = best->disc;
    copy->last_episode = best->last_episode;
    return copy;
  }
}

static int
catalog_highest_episode (BroCatalogRepository *repo, const BroContinuationQuery *query, BroBroError **error)
{
  g_autoptr (GPtrArray) all = candidates (STORE (repo), -1, error);
  int best = -1;
  for (guint i = 0; all && i < all->len; i++)
    {
      BroArchivedDisc *c = all->pdata[i];
      if (bro_episode_continuation_same_set (c, query) && c->last_episode > best)
        best = c->last_episode;
    }
  return best;
}

static gpointer
map_work (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroWorkRecord *w = bro_work_record_new ();
  w->id = col_id (r, 0);
  bro_media_kind_from_wire ((const char *) sqlite3_column_text (r, 1), &w->kind);
  w->title = col_text (r, 2);
  OPT_INT (w->has_year, w->year, r, 3);
  OPT_INT (w->has_tmdb_id, w->tmdb_id, r, 4);
  w->imdb_id = col_text_or_null (r, 5);
  w->created_at = col_time (r, 6);
  w->updated_at = col_time (r, 7);
  return w;
}

static GPtrArray *
catalog_works (BroCatalogRepository *repo, const char *query, BroBroError **error)
{
  Arg a[] = { a_text (query) };
  return locked_rows (STORE (repo),
                      "SELECT id, kind, title, year, tmdb_id, imdb_id, created_at, updated_at FROM works WHERE instr(lower(title), lower(?)) > 0 "
                      "ORDER BY title COLLATE NOCASE, id",
                      a, N (a), map_work, (GDestroyNotify) bro_work_record_free, error);
}

static gpointer
map_set (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroDiscSetRecord *s = bro_disc_set_record_new ();
  s->id = col_id (r, 0);
  OPT_ID (s->has_work_id, s->work_id, r, 1);
  s->label_title = col_text (r, 2);
  OPT_INT (s->has_season, s->season, r, 3);
  OPT_INT (s->has_part, s->part, r, 4);
  OPT_INT (s->has_volume, s->volume, r, 5);
  s->description = col_text (r, 6);
  OPT_INT (s->has_known_count, s->known_count, r, 7);
  return s;
}

static BroDiscSetRecord *
catalog_set (BroCatalogRepository *repo, BroId set_id, BroBroError **error)
{
  Arg a[] = { a_id (&set_id) };
  return first_row (locked_rows (STORE (repo),
                                 "SELECT id, work_id, label_title, season, part, volume, description, known_count FROM disc_sets WHERE id = ?",
                                 a, N (a), map_set, (GDestroyNotify) bro_disc_set_record_free, error));
}

static void
catalog_iface_init (BroCatalogRepositoryInterface *iface)
{
  iface->archived_before = catalog_archived_before;
  iface->previous_disc = catalog_previous_disc;
  iface->highest_episode = catalog_highest_episode;
  iface->works = catalog_works;
  iface->set = catalog_set;
}

/* ---- checks ---- */

#define CHECK_COLUMNS "id, unit_id, replica_id, folder, job_id, started_at, finished_at, result, files, bytes, changed, unreadable, missing, " \
                      "unlisted, error"

static gpointer
map_check (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroCheckRecord *c = bro_check_record_new ();
  g_autoptr (BroJsonValue) error = col_json (r, 14);
  c->id = col_id (r, 0);
  OPT_ID (c->has_unit_id, c->unit_id, r, 1);
  OPT_ID (c->has_replica_id, c->replica_id, r, 2);
  c->folder = col_text (r, 3);
  OPT_ID (c->has_job_id, c->job_id, r, 4);
  c->started_at = col_time (r, 5);
  OPT_TIME (c->has_finished_at, c->finished_at, r, 6);
  bro_check_result_from_wire ((const char *) sqlite3_column_text (r, 7), &c->result);
  c->files = (int) col_int (r, 8);
  c->bytes = col_int (r, 9);
  g_strfreev (c->changed);
  c->changed = strings_of (col_json (r, 10));
  g_strfreev (c->unreadable);
  c->unreadable = strings_of (col_json (r, 11));
  g_strfreev (c->missing);
  c->missing = strings_of (col_json (r, 12));
  g_strfreev (c->unlisted);
  c->unlisted = strings_of (col_json (r, 13));
  c->error = error ? message_of (error) : NULL;
  return c;
}

static gboolean
check_insert (BroCheckRepository *repo, const BroCheckRecord *c, BroBroError **error)
{
  Arg a[] = { a_id (&c->id), a_opt_id (c->has_unit_id, &c->unit_id), a_opt_id (c->has_replica_id, &c->replica_id), a_text (c->folder),
              a_opt_id (c->has_job_id, &c->job_id), a_time (c->started_at), a_opt_time (c->has_finished_at, c->finished_at),
              a_text (bro_check_result_to_wire (c->result)), a_int (c->files), a_int (c->bytes), a_json (strings_json (c->changed)),
              a_json (strings_json (c->unreadable)), a_json (strings_json (c->missing)), a_json (strings_json (c->unlisted)),
              a_json (c->error ? message_json (c->error) : NULL) };
  return locked_execute (STORE (repo), "INSERT INTO checks (" CHECK_COLUMNS ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", a, N (a), error);
}

static GPtrArray *
check_for_unit (BroCheckRepository *repo, BroId unit_id, BroBroError **error)
{
  Arg a[] = { a_id (&unit_id) };
  return locked_rows (STORE (repo), "SELECT " CHECK_COLUMNS " FROM checks WHERE unit_id = ? ORDER BY started_at DESC, id DESC", a, N (a), map_check,
                      (GDestroyNotify) bro_check_record_free, error);
}

static BroCheckRecord *
check_latest (BroCheckRepository *repo, const char *folder, BroBroError **error)
{
  Arg a[] = { a_text (folder) };
  return first_row (locked_rows (STORE (repo),
                                 "SELECT " CHECK_COLUMNS " FROM checks WHERE folder = ? ORDER BY coalesce(finished_at, started_at) DESC, id DESC LIMIT 1",
                                 a, N (a), map_check, (GDestroyNotify) bro_check_record_free, error));
}

static void
checks_iface_init (BroCheckRepositoryInterface *iface)
{
  iface->insert = check_insert;
  iface->for_unit = check_for_unit;
  iface->latest = check_latest;
}

/* ---- replicas ---- */

static gpointer
map_replica (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroReplicaRecord *x = bro_replica_record_new ();
  g_autoptr (BroJsonValue) error = col_json (r, 6);
  x->id = col_id (r, 0);
  x->unit_id = col_id (r, 1);
  x->target_id = col_text (r, 2);
  x->path = col_text (r, 3);
  bro_replica_state_from_wire ((const char *) sqlite3_column_text (r, 4), &x->state);
  OPT_TIME (x->has_verified_at, x->verified_at, r, 5);
  x->error = error ? message_of (error) : NULL;
  return x;
}

static gboolean
replica_upsert (BroReplicaRepository *repo, const BroReplicaRecord *x, BroBroError **error)
{
  Arg a[] = { a_id (&x->id), a_id (&x->unit_id), a_text (x->target_id), a_text (x->path), a_text (bro_replica_state_to_wire (x->state)),
              a_opt_time (x->has_verified_at, x->verified_at), a_json (x->error ? message_json (x->error) : NULL) };
  return locked_execute (STORE (repo),
                         "INSERT INTO replicas (id, unit_id, target_id, path, state, verified_at, error) VALUES (?, ?, ?, ?, ?, ?, ?) "
                         "ON CONFLICT (id) DO UPDATE SET unit_id = excluded.unit_id, target_id = excluded.target_id, path = excluded.path, "
                         "state = excluded.state, verified_at = excluded.verified_at, error = excluded.error",
                         a, N (a), error);
}

static GPtrArray *
replica_for_unit (BroReplicaRepository *repo, BroId unit_id, BroBroError **error)
{
  Arg a[] = { a_id (&unit_id) };
  return locked_rows (STORE (repo),
                      "SELECT id, unit_id, target_id, path, state, verified_at, error FROM replicas WHERE unit_id = ? ORDER BY target_id, id",
                      a, N (a), map_replica, (GDestroyNotify) bro_replica_record_free, error);
}

static GPtrArray *
replica_lagging (BroReplicaRepository *repo, BroBroError **error)
{
  return locked_rows (STORE (repo), "SELECT id, unit_id, target_id, path, state, verified_at, error FROM replicas WHERE state <> 'verified' ORDER BY id",
                      NULL, 0, map_replica, (GDestroyNotify) bro_replica_record_free, error);
}

static void
replicas_iface_init (BroReplicaRepositoryInterface *iface)
{
  iface->upsert = replica_upsert;
  iface->for_unit = replica_for_unit;
  iface->lagging = replica_lagging;
}

/* ---- drives ---- */

static gboolean
drive_upsert (BroDriveRepository *repo, const BroDriveRecord *d, BroBroError **error)
{
  Arg a[] = { a_text (d->id), a_text (d->identification), a_text (d->model ? d->model : ""), a_text (d->last_device ? d->last_device : ""),
              a_text (d->config_id), a_time (d->first_seen_at), a_time (d->last_seen_at) };
  return locked_execute (STORE (repo),
                         "INSERT INTO drives (id, identification, model, last_device, config_id, first_seen_at, last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?) "
                         "ON CONFLICT (id) DO UPDATE SET identification = excluded.identification, model = excluded.model, "
                         "last_device = excluded.last_device, config_id = excluded.config_id, last_seen_at = excluded.last_seen_at",
                         a, N (a), error);
}

static gpointer
map_drive (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroDriveRecord *d = bro_drive_record_new ();
  d->id = col_text (r, 0);
  d->identification = col_text (r, 1);
  d->model = col_text (r, 2);
  d->last_device = col_text (r, 3);
  d->config_id = col_text_or_null (r, 4);
  d->first_seen_at = col_time (r, 5);
  d->last_seen_at = col_time (r, 6);
  return d;
}

static GPtrArray *
drive_all (BroDriveRepository *repo, BroBroError **error)
{
  return locked_rows (STORE (repo), "SELECT id, identification, model, last_device, config_id, first_seen_at, last_seen_at FROM drives ORDER BY id",
                      NULL, 0, map_drive, (GDestroyNotify) bro_drive_record_free, error);
}

static gboolean
drive_record_stats (BroDriveRepository *repo, const char *drive_id, const char *day, const BroDriveStats *s, BroBroError **error)
{
  Arg a[] = { a_text (drive_id), a_text (day), a_int (s->jobs), a_int (s->failed_jobs), a_int (s->read_error_jobs), a_int (s->read_errors),
              a_int (s->bytes_read), a_double (s->seconds_reading) };
  return locked_execute (STORE (repo),
                         "INSERT INTO drive_stats_daily (drive_id, day, jobs, failed_jobs, read_error_jobs, read_errors, bytes_read, seconds_reading) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (drive_id, day) DO UPDATE SET jobs = jobs + excluded.jobs, "
                         "failed_jobs = failed_jobs + excluded.failed_jobs, read_error_jobs = read_error_jobs + excluded.read_error_jobs, "
                         "read_errors = read_errors + excluded.read_errors, bytes_read = bytes_read + excluded.bytes_read, "
                         "seconds_reading = seconds_reading + excluded.seconds_reading",
                         a, N (a), error);
}

static void
drives_iface_init (BroDriveRepositoryInterface *iface)
{
  iface->upsert = drive_upsert;
  iface->all = drive_all;
  iface->record_stats = drive_record_stats;
}

/* ---- lookup cache ---- */

static gpointer
map_response (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroHttpResponse *h = bro_http_response_new ();
  h->status = (int) col_int (r, 0);
  g_clear_pointer (&h->body, g_bytes_unref);
  h->body = g_bytes_new (sqlite3_column_blob (r, 1), (gsize) sqlite3_column_bytes (r, 1));
  return h;
}

static BroHttpResponse *
lookup_get (BroLookupCacheRepository *repo, const char *provider, const char *key, BroBroError **error)
{
  BroSqliteStore *self = STORE (repo);
  Arg a[] = { a_text (provider), a_text (key), a_time (bro_clock_now (self->shared->clock)) };
  return first_row (locked_rows (self, "SELECT status, response FROM lookup_cache WHERE provider = ? AND request_key = ? AND expires_at > ?", a, N (a),
                                 map_response, (GDestroyNotify) bro_http_response_free, error));
}

static gboolean
lookup_put (BroLookupCacheRepository *repo, const char *provider, const char *key, const BroHttpResponse *response, BroInstant expires_at,
            BroBroError **error)
{
  BroSqliteStore *self = STORE (repo);
  gsize size = 0;
  gconstpointer body = response->body ? g_bytes_get_data (response->body, &size) : "";
  Arg a[] = { a_text (provider), a_text (key), a_int (response->status), a_blob (body ? body : "", size), a_time (bro_clock_now (self->shared->clock)),
              a_time (expires_at) };
  return locked_execute (self,
                         "INSERT OR REPLACE INTO lookup_cache (provider, request_key, status, response, fetched_at, expires_at) VALUES (?, ?, ?, ?, ?, ?)",
                         a, N (a), error);
}

static void
lookup_iface_init (BroLookupCacheRepositoryInterface *iface)
{
  iface->get = lookup_get;
  iface->put = lookup_put;
}

/* ---- outbox ---- */

#define OUTBOX_COLUMNS "id, target_id, job_id, title, body, status, created_at, attempts, next_attempt_at, sent_at, last_error"

static gpointer
map_outbox (BroSqliteStore *self, sqlite3_stmt *r)
{
  BroOutboxEntry *o = bro_outbox_entry_new ();
  g_autoptr (BroJsonValue) title = col_json (r, 3);
  g_autoptr (BroJsonValue) body = col_json (r, 4);
  g_autoptr (BroJsonValue) error = col_json (r, 10);
  o->id = col_id (r, 0);
  o->target_id = col_text (r, 1);
  OPT_ID (o->has_job_id, o->job_id, r, 2);
  o->title = title ? message_of (title) : bro_bro_message_new (BRO_MSG_INTERNAL_UNEXPECTED, NULL, BRO_SEVERITY_INFO);
  for (guint i = 0; body && i < bro_json_value_length (body); i++)
    g_ptr_array_add (o->lines, message_of (bro_json_value_at (body, i)));
  bro_status_word_from_wire ((const char *) sqlite3_column_text (r, 5), &o->status);
  o->created_at = col_time (r, 6);
  o->attempts = (int) col_int (r, 7);
  OPT_TIME (o->has_next_attempt_at, o->next_attempt_at, r, 8);
  OPT_TIME (o->has_sent_at, o->sent_at, r, 9);
  o->last_error = error ? error_of (error) : NULL;
  return o;
}

static gboolean
outbox_add (BroOutboxRepository *repo, const BroOutboxEntry *n, BroBroError **error)
{
  BroJsonValue *lines = bro_json_value_new_array ();
  for (guint i = 0; n->lines && i < n->lines->len; i++)
    bro_json_value_append (lines, message_json (n->lines->pdata[i]));
  {
    Arg a[] = { a_id (&n->id), a_text (n->target_id), a_opt_id (n->has_job_id, &n->job_id), a_json (message_json (n->title)), a_json (lines),
                a_text (bro_status_word_to_wire (n->status)), a_time (n->created_at), a_int (n->attempts),
                a_opt_time (n->has_next_attempt_at, n->next_attempt_at), a_opt_time (n->has_sent_at, n->sent_at),
                a_json (n->last_error ? error_json (n->last_error) : NULL) };
    return locked_execute (STORE (repo), "INSERT INTO outbox (" OUTBOX_COLUMNS ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", a, N (a), error);
  }
}

static GPtrArray *
outbox_due (BroOutboxRepository *repo, BroInstant now, BroBroError **error)
{
  Arg a[] = { a_time (now) };
  return locked_rows (STORE (repo),
                      "SELECT " OUTBOX_COLUMNS " FROM outbox WHERE sent_at IS NULL AND next_attempt_at IS NOT NULL AND next_attempt_at <= ? "
                      "ORDER BY next_attempt_at, created_at, id",
                      a, N (a), map_outbox, (GDestroyNotify) bro_outbox_entry_free, error);
}

static gboolean
outbox_mark_sent (BroOutboxRepository *repo, BroId id, BroBroError **error)
{
  BroSqliteStore *self = STORE (repo);
  Arg a[] = { a_time (bro_clock_now (self->shared->clock)), a_id (&id) };
  return locked_execute (self, "UPDATE outbox SET sent_at = ?, attempts = attempts + 1, last_error = NULL WHERE id = ?", a, N (a), error);
}

static gboolean
outbox_mark_failed (BroOutboxRepository *repo, BroId id, const BroBroError *last_error, const BroInstant *retry_at, BroBroError **error)
{
  Arg a[] = { a_json (error_json (last_error)), retry_at ? a_time (*retry_at) : a_null (), a_id (&id) };
  return locked_execute (STORE (repo), "UPDATE outbox SET attempts = attempts + 1, last_error = ?, next_attempt_at = ? WHERE id = ?", a, N (a), error);
}

static void
outbox_iface_init (BroOutboxRepositoryInterface *iface)
{
  iface->add = outbox_add;
  iface->due = outbox_due;
  iface->mark_sent = outbox_mark_sent;
  iface->mark_failed = outbox_mark_failed;
}

/* ---- key-value ---- */

static gpointer
map_kv (BroSqliteStore *self, sqlite3_stmt *r)
{
  return col_json (r, 0);
}

static BroJsonValue *
kv_get (BroKeyValueRepository *repo, const char *key, BroBroError **error)
{
  Arg a[] = { a_text (key) };
  return first_row (locked_rows (STORE (repo), "SELECT value FROM kv WHERE key = ?", a, N (a), map_kv, (GDestroyNotify) bro_json_value_unref, error));
}

static gboolean
kv_set (BroKeyValueRepository *repo, const char *key, BroJsonValue *value, BroBroError **error)
{
  BroSqliteStore *self = STORE (repo);
  Arg a[] = { a_text (key), a_json (bro_json_value_ref (value)), a_time (bro_clock_now (self->shared->clock)) };
  return locked_execute (self,
                         "INSERT INTO kv (key, value, updated_at) VALUES (?, ?, ?) ON CONFLICT (key) DO UPDATE SET value = excluded.value, "
                         "updated_at = excluded.updated_at",
                         a, N (a), error);
}

static void
kv_iface_init (BroKeyValueRepositoryInterface *iface)
{
  iface->get = kv_get;
  iface->set = kv_set;
}

/* ---- the store ---- */

static BroJobRepository *s_jobs (BroStore *s) { return BRO_JOB_REPOSITORY (s); }
static BroStepRepository *s_steps (BroStore *s) { return BRO_STEP_REPOSITORY (s); }
static BroUnitRepository *s_units (BroStore *s) { return BRO_UNIT_REPOSITORY (s); }
static BroCatalogRepository *s_catalog (BroStore *s) { return BRO_CATALOG_REPOSITORY (s); }
static BroCheckRepository *s_checks (BroStore *s) { return BRO_CHECK_REPOSITORY (s); }
static BroReplicaRepository *s_replicas (BroStore *s) { return BRO_REPLICA_REPOSITORY (s); }
static BroDriveRepository *s_drives (BroStore *s) { return BRO_DRIVE_REPOSITORY (s); }
static BroLookupCacheRepository *s_lookup (BroStore *s) { return BRO_LOOKUP_CACHE_REPOSITORY (s); }
static BroOutboxRepository *s_outbox (BroStore *s) { return BRO_OUTBOX_REPOSITORY (s); }
static BroKeyValueRepository *s_kv (BroStore *s) { return BRO_KEY_VALUE_REPOSITORY (s); }

gboolean
bro_sqlite_store_transaction (BroSqliteStore *self, BroStoreBlockFunc block, gpointer data, BroBroError **error)
{
  BroSqliteStore *view;
  gpointer outer;
  gboolean ok;
  if (self->in_transaction)
    return block (BRO_STORE (self), data, error);
  if (!check_not_reentered (self, error))
    return FALSE;
  g_mutex_lock (&self->shared->lock);
  if (!exec_sql (self, "BEGIN IMMEDIATE", error))
    {
      g_mutex_unlock (&self->shared->lock);
      return FALSE;
    }
  view = g_object_new (BRO_TYPE_SQLITE_STORE, NULL);
  view->shared = shared_ref (self->shared);
  view->in_transaction = TRUE;
  outer = g_private_get (&in_transaction_of);
  g_private_set (&in_transaction_of, self->shared);
  ok = block (BRO_STORE (view), data, error);
  g_private_set (&in_transaction_of, outer);
  if (ok)
    ok = exec_sql (self, "COMMIT", error);
  if (!ok)
    exec_sql (self, "ROLLBACK", NULL);
  g_object_unref (view);
  g_mutex_unlock (&self->shared->lock);
  return ok;
}

gboolean
bro_sqlite_store_migrate (BroSqliteStore *self, BroBroError **error)
{
  const BroMigration *m;
  sqlite3_stmt *stmt = NULL;
  int current = 0, newest = 0;
  gboolean ok = TRUE;
  if (self->in_transaction)
    return store_failed (error, "migrate", "inside a transaction");
  if (!check_not_reentered (self, error))
    return FALSE;
  g_mutex_lock (&self->shared->lock);
  if (!self->shared->db || sqlite3_prepare_v2 (self->shared->db, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK)
    ok = sql_failed (self, error);
  else
    {
      if (sqlite3_step (stmt) == SQLITE_ROW)
        current = sqlite3_column_int (stmt, 0);
      sqlite3_finalize (stmt);
    }
  for (m = _bro_migrations (); ok && m->name; m++)
    newest = MAX (newest, m->version);
  /* A database from a newer Bromelia isn't migrated as if it were current. */
  if (ok && current > newest)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "version", bro_json_value_new_integer (current));
      bro_json_value_set (params, "newest", bro_json_value_new_integer (newest));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_STORE_TOO_NEW), params);
      ok = FALSE;
    }
  for (m = _bro_migrations (); ok && m->name; m++)
    {
      if (m->version <= current)
        continue;
      ok = exec_sql (self, "BEGIN", error);
      if (ok)
        ok = exec_sql (self, m->sql, error);
      if (ok)
        ok = exec_sql (self, "COMMIT", error);
      else
        exec_sql (self, "ROLLBACK", NULL);
    }
  g_mutex_unlock (&self->shared->lock);
  return ok;
}

static gboolean s_transaction (BroStore *s, BroStoreBlockFunc block, gpointer data, BroBroError **error)
{
  return bro_sqlite_store_transaction (STORE (s), block, data, error);
}

static gboolean s_migrate (BroStore *s, BroBroError **error) { return bro_sqlite_store_migrate (STORE (s), error); }

static void
store_iface_init (BroStoreInterface *iface)
{
  iface->jobs = s_jobs;
  iface->steps = s_steps;
  iface->units = s_units;
  iface->catalog = s_catalog;
  iface->checks = s_checks;
  iface->replicas = s_replicas;
  iface->drives = s_drives;
  iface->lookup_cache = s_lookup;
  iface->outbox = s_outbox;
  iface->kv = s_kv;
  iface->transaction = s_transaction;
  iface->migrate = s_migrate;
}

void
bro_sqlite_store_close (BroSqliteStore *self)
{
  if (self->in_transaction || !self->shared)
    return;
  g_mutex_lock (&self->shared->lock);
  if (self->shared->db)
    sqlite3_close_v2 (self->shared->db);
  self->shared->db = NULL;
  g_mutex_unlock (&self->shared->lock);
}

static void
bro_sqlite_store_finalize (GObject *object)
{
  BroSqliteStore *self = STORE (object);
  if (self->shared)
    shared_unref (self->shared);
  G_OBJECT_CLASS (bro_sqlite_store_parent_class)->finalize (object);
}

static void
bro_sqlite_store_class_init (BroSqliteStoreClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_sqlite_store_finalize;
}

static void
bro_sqlite_store_init (BroSqliteStore *self)
{
}

static gpointer map_text (BroSqliteStore *self, sqlite3_stmt *r) { return col_text (r, 0); }

BroSqliteStore *
bro_sqlite_store_open (const char *path, BroClock *clock, BroBroError **error)
{
  sqlite3 *db = NULL;
  BroSqliteStore *self;
  int rc = sqlite3_open_v2 (path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK)
    {
      store_failed (error, "open", db ? sqlite3_errmsg (db) : sqlite3_errstr (rc));
      if (db)
        sqlite3_close_v2 (db);
      return NULL;
    }
  self = g_object_new (BRO_TYPE_SQLITE_STORE, NULL);
  self->shared = g_new0 (Shared, 1);
  g_atomic_ref_count_init (&self->shared->refs);
  g_mutex_init (&self->shared->lock);
  self->shared->db = db;
  self->shared->clock = g_object_ref (clock);
  {
    /* SQLite answers with the mode it is in: anything but WAL (a network share, a read-only folder) isn't safe here. */
    g_autoptr (GPtrArray) mode = query_rows (self, "PRAGMA journal_mode=WAL", NULL, 0, map_text, g_free, error);
    const char *got = mode && mode->len ? mode->pdata[0] : "";
    if (!mode)
      {
        g_object_unref (self);
        return NULL;
      }
    if (!g_str_equal (got, "wal") && !(g_str_equal (path, ":memory:") && g_str_equal (got, "memory")))
      {
        g_autofree char *reason = g_strdup_printf ("the journal is %s, not WAL", got);
        store_failed (error, "open", reason);
        g_object_unref (self);
        return NULL;
      }
  }
  if (!exec_sql (self, "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;", error))
    {
      g_object_unref (self);
      return NULL;
    }
  return self;
}
