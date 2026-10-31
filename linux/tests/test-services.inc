/* test-services.inc: shared/fixtures/engine/problem-service.cases.json and config-service.cases.json, on a real
 * executor (included by test-engine.c). */
#include "bro-config-codec.h"
#include "bro-config-service.h"
#include "bro-problem-service.h"

#define CONFIG_PATH "/data/config.json"

/* Records every event a bus publishes (subscribed on the executor). */
typedef struct {
  BroEventBus *bus;
  BroSubscription *subscription;
  GPtrArray *events; /* BroEngineEvent */
} SubscribeHarness;

static void
subscribe_action (gpointer data)
{
  SubscribeHarness *s = data;
  s->subscription = bro_event_bus_subscribe (s->bus, NULL, 0, event_seen, s->events, NULL);
}

static void
subscribe_harness_init (SubscribeHarness *s, BroEngineExecutor *executor, BroEventBus *bus)
{
  s->bus = bus;
  s->subscription = NULL;
  s->events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_engine_event_unref);
  bro_engine_executor_post (executor, subscribe_action, s, NULL);
  bro_engine_executor_barrier (executor);
}

/* After the executor has stopped. */
static void
subscribe_harness_clear (SubscribeHarness *s)
{
  g_clear_object (&s->subscription);
  g_ptr_array_unref (s->events);
}

/* The value with every object's keys sorted, to compare documents whose key order doesn't matter. */
static BroJsonValue *
sorted_json (const BroJsonValue *v)
{
  if (!v)
    return bro_json_value_new_null ();
  if (v->kind == BRO_JSON_VALUE_ARRAY)
    {
      BroJsonValue *a = bro_json_value_new_array ();
      for (guint i = 0; i < v->items->len; i++)
        bro_json_value_append (a, sorted_json (v->items->pdata[i]));
      return a;
    }
  if (v->kind == BRO_JSON_VALUE_OBJECT)
    {
      BroJsonValue *o = bro_json_value_new_object ();
      g_autoptr (GPtrArray) keys = g_ptr_array_new ();
      for (guint i = 0; i < v->keys->len; i++)
        g_ptr_array_add (keys, v->keys->pdata[i]);
      g_ptr_array_sort (keys, compare_strings_in_array);
      for (guint i = 0; i < keys->len; i++)
        bro_json_value_set (o, keys->pdata[i], sorted_json (bro_json_value_member (v, keys->pdata[i])));
      return o;
    }
  return bro_json_value_ref ((BroJsonValue *) v);
}

static void
same_sorted (GPtrArray *failures, const char *id, const char *what, const BroJsonValue *expected, const BroJsonValue *actual)
{
  g_autoptr (BroJsonValue) e = sorted_json (expected);
  g_autoptr (BroJsonValue) a = sorted_json (actual);
  bro_test_same_json (failures, id, what, e, a);
}

static BroProblemScope *
scope_of (BroJsonValue *j)
{
  BroProblemScopeKind kind;
  if (!j || !bro_problem_scope_kind_from_wire (bro_json_value_get_string (bro_json_value_member (j, "kind"), ""), &kind))
    return NULL;
  return bro_problem_scope_new (kind, bro_json_value_get_string (bro_json_value_member (j, "id"), ""));
}

static BroJsonValue *
problems_json (BroProblemService *problems)
{
  g_autoptr (GPtrArray) list = bro_problem_service_problems (problems);
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < list->len; i++)
    bro_json_value_append (a, bro_problem_dto_json (list->pdata[i]));
  return a;
}

static BroJsonValue *
events_json (GPtrArray *events)
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < events->len; i++)
    bro_json_value_append (a, bro_engine_event_json (events->pdata[i]));
  return a;
}

/* ---- problem-service.cases.json ---- */

typedef struct {
  BroProblemService *problems;
  BroJsonValue *op;
} ProblemOp;

static void
problem_op_action (gpointer data)
{
  ProblemOp *o = data;
  BroJsonValue *arg = o->op->items->pdata[0];
  const char *name = o->op->keys->pdata[0];
  BroMessageCode code;
  g_autoptr (BroProblemScope) scope = scope_of (bro_json_value_member (arg, "scope"));
  bro_message_code_parse (bro_json_value_get_string (bro_json_value_member (arg, "code"), ""), &code);
  if (strcmp (name, "raise") == 0)
    {
      BroJsonValue *params = bro_json_value_member (arg, "params");
      BroJsonValue *actions = bro_json_value_member (arg, "actions");
      g_autoptr (GArray) list = g_array_new (FALSE, FALSE, sizeof (BroProblemAction));
      BroSeverity severity = BRO_SEVERITY_INFO;
      g_autoptr (BroBroMessage) message = NULL;
      g_autoptr (BroProblemDto) raised = NULL;
      bro_severity_from_wire (bro_json_value_get_string (bro_json_value_member (arg, "severity"), ""), &severity);
      for (guint i = 0; i < bro_json_value_length (actions); i++)
        {
          BroProblemAction a;
          if (bro_problem_action_from_wire (bro_json_value_get_string (bro_json_value_at (actions, i), ""), &a))
            g_array_append_val (list, a);
        }
      message = bro_bro_message_new (code, params ? bro_json_value_ref (params) : NULL, severity);
      raised = bro_problem_service_raise (o->problems, message, scope, (BroProblemAction *) list->data, list->len);
    }
  else if (strcmp (name, "clear") == 0)
    bro_problem_service_clear (o->problems, code, scope);
}

static gboolean
problem_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  World w;
  BroEventBus *bus;
  BroProblemService *problems;
  SubscribeHarness s;
  BroJsonValue *ops = bro_json_value_member (given, "ops");
  g_autoptr (BroJsonValue) got_problems = NULL;
  g_autoptr (BroJsonValue) got_events = NULL;
  world_init (&w);
  bus = bro_event_bus_new (BRO_CLOCK (w.clock), 1, 1024, 1024);
  problems = bro_problem_service_new (bus, BRO_CLOCK (w.clock));
  subscribe_harness_init (&s, w.executor, bus);
  for (guint i = 0; i < bro_json_value_length (ops); i++)
    {
      BroJsonValue *op = bro_json_value_at (ops, i);
      if (strcmp (op->keys->pdata[0], "advance") == 0)
        test_manual_clock_advance (w.clock, bro_json_value_get_integer (op->items->pdata[0], 0) * 1000, settle, &w);
      else
        {
          ProblemOp o = { problems, op };
          bro_engine_executor_post (w.executor, problem_op_action, &o, NULL);
          bro_engine_executor_barrier (w.executor);
        }
    }
  got_problems = problems_json (problems);
  got_events = events_json (s.events);
  bro_test_same_json (failures, id, "problems", bro_json_value_member (expect, "problems"), got_problems);
  bro_test_same_json (failures, id, "events", bro_json_value_member (expect, "events"), got_events);
  bro_engine_executor_stop (w.executor);
  subscribe_harness_clear (&s);
  g_object_unref (problems);
  g_object_unref (bus);
  world_clear (&w);
  return TRUE;
}

static void
test_problem_service (void)
{
  bro_test_run_cases ("engine/problem-service.cases.json", problem_case);
}

/* ---- config-service.cases.json: config.json, the keystore and the kv table, in memory ---- */

#define TEST_TYPE_CONFIG_FAKES (test_config_fakes_get_type ())
G_DECLARE_FINAL_TYPE (TestConfigFakes, test_config_fakes, TEST, CONFIG_FAKES, GObject)

struct _TestConfigFakes {
  GObject parent_instance;
  GMutex lock;
  GCond cond;
  GBytes *file;            /* NULL: none */
  int mode;                /* -1: never written */
  BroJsonValue *write_fails;
  gboolean hold, held, released;
  GHashTable *keystore;    /* name → value */
  BroJsonValue *keystore_fails;
  GHashTable *kv;          /* key → BroJsonValue */
  gboolean system_store;
};

static void fakes_fs_iface (BroFileSystemInterface *iface);
static void fakes_keystore_iface (BroKeystoreInterface *iface);
static void fakes_kv_iface (BroKeyValueRepositoryInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestConfigFakes, test_config_fakes, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, fakes_fs_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_KEYSTORE, fakes_keystore_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_KEY_VALUE_REPOSITORY, fakes_kv_iface))

static BroBroError *
failure_of (BroJsonValue *j)
{
  BroJsonValue *params = bro_json_value_member (j, "params");
  return bro_bro_error_new (bro_json_value_get_string (bro_json_value_member (j, "code"), ""), params ? bro_json_value_ref (params) : NULL,
                            NULL);
}

static gboolean
fakes_exists (BroFileSystem *fs, const char *path)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (fs);
  gboolean there;
  g_mutex_lock (&f->lock);
  there = strcmp (path, CONFIG_PATH) == 0 && f->file != NULL;
  g_mutex_unlock (&f->lock);
  return there;
}

static GBytes *
fakes_read (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (fs);
  GBytes *bytes;
  g_mutex_lock (&f->lock);
  bytes = f->file ? g_bytes_ref (f->file) : NULL;
  g_mutex_unlock (&f->lock);
  if (!bytes)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (path));
      bro_bro_error_set (error, "fs.notFound", params);
    }
  return bytes;
}

/* With holdWrites, every write (kv or file) waits here for releaseWrites. Called with the lock held. */
static gboolean
wait_if_held (TestConfigFakes *f, BroBroError **error)
{
  gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  if (!f->hold)
    return TRUE;
  f->held = TRUE;
  g_cond_broadcast (&f->cond);
  while (!f->released)
    if (!g_cond_wait_until (&f->cond, &f->lock, until))
      {
        bro_bro_error_set (error, "test.neverReleased", NULL);
        return FALSE;
      }
  return TRUE;
}

static gboolean
fakes_write_atomically (BroFileSystem *fs, const char *path, GBytes *bytes, int mode, BroBroError **error)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (fs);
  g_assert_cmpstr (path, ==, CONFIG_PATH);
  g_mutex_lock (&f->lock);
  if (!wait_if_held (f, error))
    {
      g_mutex_unlock (&f->lock);
      return FALSE;
    }
  if (f->write_fails)
    {
      g_mutex_unlock (&f->lock);
      *error = failure_of (f->write_fails);
      return FALSE;
    }
  g_clear_pointer (&f->file, g_bytes_unref);
  f->file = g_bytes_ref (bytes);
  f->mode = mode;
  g_mutex_unlock (&f->lock);
  return TRUE;
}

static void
fakes_fs_iface (BroFileSystemInterface *iface)
{
  iface->exists = fakes_exists;
  iface->read = fakes_read;
  iface->write_atomically = fakes_write_atomically;
}

static gboolean
keystore_check (TestConfigFakes *f, const char *name, BroBroError **error)
{
  BroJsonValue *fails = bro_json_value_member (f->keystore_fails, name);
  if (!fails)
    return TRUE;
  *error = failure_of (fails);
  return FALSE;
}

static char *
fakes_keystore_get (BroKeystore *keystore, const char *name, BroBroError **error)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (keystore);
  char *value;
  if (!keystore_check (f, name, error))
    return NULL;
  g_mutex_lock (&f->lock);
  value = g_strdup (g_hash_table_lookup (f->keystore, name));
  g_mutex_unlock (&f->lock);
  return value;
}

static gboolean
fakes_keystore_set (BroKeystore *keystore, const char *name, const char *value, BroBroError **error)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (keystore);
  if (!keystore_check (f, name, error))
    return FALSE;
  g_mutex_lock (&f->lock);
  g_hash_table_replace (f->keystore, g_strdup (name), g_strdup (value));
  g_mutex_unlock (&f->lock);
  return TRUE;
}

static gboolean
fakes_keystore_remove (BroKeystore *keystore, const char *name, BroBroError **error)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (keystore);
  if (!keystore_check (f, name, error))
    return FALSE;
  g_mutex_lock (&f->lock);
  g_hash_table_remove (f->keystore, name);
  g_mutex_unlock (&f->lock);
  return TRUE;
}

static gboolean
fakes_keystore_is_system_store (BroKeystore *keystore)
{
  return TEST_CONFIG_FAKES (keystore)->system_store;
}

static void
fakes_keystore_iface (BroKeystoreInterface *iface)
{
  iface->is_system_store = fakes_keystore_is_system_store;
  iface->get = fakes_keystore_get;
  iface->set = fakes_keystore_set;
  iface->remove = fakes_keystore_remove;
}

static BroJsonValue *
fakes_kv_get (BroKeyValueRepository *kv, const char *key, BroBroError **error)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (kv);
  BroJsonValue *value;
  g_mutex_lock (&f->lock);
  value = g_hash_table_lookup (f->kv, key);
  value = value ? bro_json_value_ref (value) : NULL;
  g_mutex_unlock (&f->lock);
  return value;
}

static gboolean
fakes_kv_set (BroKeyValueRepository *kv, const char *key, BroJsonValue *value, BroBroError **error)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (kv);
  g_mutex_lock (&f->lock);
  if (!wait_if_held (f, error))
    {
      g_mutex_unlock (&f->lock);
      return FALSE;
    }
  g_hash_table_replace (f->kv, g_strdup (key), bro_json_value_ref (value));
  g_mutex_unlock (&f->lock);
  return TRUE;
}

static void
fakes_kv_iface (BroKeyValueRepositoryInterface *iface)
{
  iface->get = fakes_kv_get;
  iface->set = fakes_kv_set;
}

static void
test_config_fakes_finalize (GObject *object)
{
  TestConfigFakes *f = TEST_CONFIG_FAKES (object);
  g_clear_pointer (&f->file, g_bytes_unref);
  g_clear_pointer (&f->write_fails, bro_json_value_unref);
  g_clear_pointer (&f->keystore_fails, bro_json_value_unref);
  g_hash_table_unref (f->keystore);
  g_hash_table_unref (f->kv);
  g_mutex_clear (&f->lock);
  g_cond_clear (&f->cond);
  G_OBJECT_CLASS (test_config_fakes_parent_class)->finalize (object);
}

static void
test_config_fakes_class_init (TestConfigFakesClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_config_fakes_finalize;
}

static void
test_config_fakes_init (TestConfigFakes *f)
{
  g_mutex_init (&f->lock);
  g_cond_init (&f->cond);
  f->mode = -1;
  f->keystore = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  f->kv = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_json_value_unref);
}

/* ---- the harness ---- */

typedef struct {
  BroConfigService *service;
  GMutex lock;
  GCond cond;
  BroJsonValue **results; /* one per op */
  gboolean *done;         /* one per op */
} ConfigHarness;

typedef struct {
  ConfigHarness *h;
  guint index;
  BroJsonValue *op;
} ConfigOp;

static BroJsonValue *
error_json (const BroBroError *error)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *e = bro_json_value_new_object ();
  bro_json_value_set (e, "code", bro_json_value_new_string (error->code));
  bro_json_value_set (e, "params", bro_json_value_ref (error->params));
  bro_json_value_set (o, "error", e);
  return o;
}

static void
record (ConfigHarness *h, guint index, BroJsonValue *result)
{
  g_mutex_lock (&h->lock);
  h->results[index] = result;
  h->done[index] = TRUE;
  g_cond_broadcast (&h->cond);
  g_mutex_unlock (&h->lock);
}

static void
wait_for (ConfigHarness *h, guint index)
{
  gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  g_mutex_lock (&h->lock);
  while (!h->done[index])
    if (!g_cond_wait_until (&h->cond, &h->lock, until))
      g_error ("op %u didn't finish", index);
  g_mutex_unlock (&h->lock);
}

static void
config_done (const BroBroError *error, gpointer data)
{
  ConfigOp *o = data;
  record (o->h, o->index, error ? error_json (error) : bro_json_value_new_string ("ok"));
}

static void
envelope_done (BroConfigEnvelope *envelope, const BroBroError *error, gpointer data)
{
  ConfigOp *o = data;
  record (o->h, o->index, error ? error_json (error) : bro_config_envelope_json (envelope));
  bro_config_envelope_free (envelope);
}

static BroJsonValue *
issues_json (GPtrArray *issues)
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < issues->len; i++)
    bro_json_value_append (a, bro_issue_to_json (issues->pdata[i]));
  return a;
}

static void
config_op_action (gpointer data)
{
  ConfigOp *o = data;
  ConfigHarness *h = o->h;
  const char *name = o->op->kind == BRO_JSON_VALUE_STRING ? o->op->string : o->op->keys->pdata[0];
  BroJsonValue *arg = o->op->kind == BRO_JSON_VALUE_OBJECT ? o->op->items->pdata[0] : NULL;
  ConfigOp *copy = g_memdup2 (o, sizeof *o);
  if (strcmp (name, "load") == 0)
    bro_config_service_load (h->service, config_done, copy, g_free);
  else if (strcmp (name, "update") == 0)
    bro_config_service_update (h->service, bro_json_value_member (arg, "doc"),
                               bro_json_value_get_integer (bro_json_value_member (arg, "version"), -1), envelope_done, copy, g_free);
  else if (strcmp (name, "envelope") == 0)
    bro_config_service_envelope (h->service, envelope_done, copy, g_free);
  else if (strcmp (name, "setSecret") == 0)
    bro_config_service_set_secret (h->service, bro_json_value_get_string (bro_json_value_member (arg, "name"), ""),
                                   bro_json_value_get_string (bro_json_value_member (arg, "value"), ""), config_done, copy, g_free);
  else
    {
      g_autoptr (BroBroError) error = NULL;
      BroJsonValue *result = NULL;
      g_free (copy);
      if (strcmp (name, "validate") == 0)
        {
          g_autoptr (GPtrArray) issues = bro_config_service_validate (h->service, arg, &error);
          result = issues ? issues_json (issues) : error_json (error);
        }
      else if (strcmp (name, "current") == 0)
        {
          g_autoptr (BroConfig) c = bro_config_service_current (h->service);
          result = bro_json_value_ref (c->document);
        }
      else if (strcmp (name, "version") == 0)
        result = bro_json_value_new_integer (bro_config_service_version (h->service));
      else if (strcmp (name, "export") == 0)
        {
          g_autoptr (GBytes) bundle = bro_config_service_export (h->service);
          gsize size;
          const char *text = g_bytes_get_data (bundle, &size);
          result = bro_json_value_parse (text, (gssize) size);
        }
      else
        g_error ("unknown op %s", name);
      record (h, o->index, result);
    }
}

/* A document as bro_config_codec_decode leaves it, so a case can write it short. */
static BroJsonValue *
norm (const BroJsonValue *doc)
{
  g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (doc);
  gsize size;
  const char *text = g_bytes_get_data (bytes, &size);
  g_autoptr (BroConfig) c = bro_config_codec_decode (text, (gssize) size, NULL);
  return c ? bro_json_value_ref (c->document) : bro_json_value_ref ((BroJsonValue *) doc);
}

/* @obj with @key's value replaced by norm of it. */
static BroJsonValue *
norm_member (const BroJsonValue *obj, const char *key)
{
  BroJsonValue *o = bro_json_value_new_object ();
  for (guint i = 0; i < obj->keys->len; i++)
    {
      const char *k = obj->keys->pdata[i];
      bro_json_value_set (o, k, strcmp (k, key) == 0 ? norm (obj->items->pdata[i]) : bro_json_value_ref (obj->items->pdata[i]));
    }
  return o;
}

static gboolean
config_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  World w;
  BroEventBus *bus;
  BroProblemService *problems;
  TestConfigFakes *f = g_object_new (TEST_TYPE_CONFIG_FAKES, NULL);
  TestConfigFakes *folder = g_object_new (TEST_TYPE_CONFIG_FAKES, NULL); /* the secrets folder: only its keystore part */
  gboolean system_store = bro_json_value_get_bool (bro_json_value_member (given, "systemStore"), TRUE);
  ConfigHarness h = { 0 };
  SubscribeHarness s;
  BroJsonValue *ops = bro_json_value_member (given, "ops"), *v, *want_results = bro_json_value_member (expect, "results");
  g_autoptr (GHashTable) environment = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GArray) started = g_array_new (FALSE, FALSE, sizeof (guint));
  guint n = bro_json_value_length (ops), r = 0;

  world_init (&w);
  if ((v = bro_json_value_member (given, "file")))
    {
      const char *text = bro_json_value_get_string (bro_json_value_member (v, "text"), NULL);
      f->file = text ? g_bytes_new (text, strlen (text)) : bro_json_value_encode_canonical (v);
    }
  if ((v = bro_json_value_member (given, "writeFails")))
    f->write_fails = bro_json_value_ref (v);
  f->hold = bro_json_value_get_bool (bro_json_value_member (given, "holdWrites"), FALSE);
  if ((v = bro_json_value_member (given, "keystore")))
    for (guint i = 0; i < v->keys->len; i++)
      g_hash_table_insert (f->keystore, g_strdup (v->keys->pdata[i]), g_strdup (bro_json_value_get_string (v->items->pdata[i], "")));
  f->keystore_fails = (v = bro_json_value_member (given, "keystoreFails")) ? bro_json_value_ref (v) : bro_json_value_new_object ();
  f->system_store = TRUE;
  if ((v = bro_json_value_member (given, "files")))
    for (guint i = 0; i < v->keys->len; i++)
      g_hash_table_insert (folder->keystore, g_strdup (v->keys->pdata[i]), g_strdup (bro_json_value_get_string (v->items->pdata[i], "")));
  folder->keystore_fails = (v = bro_json_value_member (given, "filesFail")) ? bro_json_value_ref (v) : bro_json_value_new_object ();
  folder->system_store = FALSE;
  if ((v = bro_json_value_member (given, "kv")))
    for (guint i = 0; i < v->keys->len; i++)
      g_hash_table_insert (f->kv, g_strdup (v->keys->pdata[i]), bro_json_value_ref (v->items->pdata[i]));
  if ((v = bro_json_value_member (given, "environment")))
    for (guint i = 0; i < v->keys->len; i++)
      g_hash_table_insert (environment, v->keys->pdata[i], (gpointer) bro_json_value_get_string (v->items->pdata[i], ""));

  bus = bro_event_bus_new (BRO_CLOCK (w.clock), 1, 1024, 1024);
  problems = bro_problem_service_new (bus, BRO_CLOCK (w.clock));
  /* Without a system store, the keystore is the secrets folder. */
  h.service = bro_config_service_new (w.executor, bus, problems, BRO_FILE_SYSTEM (f), system_store ? BRO_KEYSTORE (f) : BRO_KEYSTORE (folder),
                                      BRO_KEYSTORE (folder), BRO_KEY_VALUE_REPOSITORY (f), CONFIG_PATH, environment);
  g_mutex_init (&h.lock);
  g_cond_init (&h.cond);
  h.results = g_new0 (BroJsonValue *, n);
  h.done = g_new0 (gboolean, n);
  subscribe_harness_init (&s, w.executor, bus);

  for (guint i = 0; i < n; i++)
    {
      BroJsonValue *op = bro_json_value_at (ops, i);
      const char *name = op->kind == BRO_JSON_VALUE_STRING ? op->string : op->keys->pdata[0];
      if (strcmp (name, "releaseWrites") == 0)
        {
          g_mutex_lock (&f->lock);
          f->released = TRUE;
          g_cond_broadcast (&f->cond);
          g_mutex_unlock (&f->lock);
          for (guint k = 0; k < started->len; k++)
            wait_for (&h, g_array_index (started, guint, k));
        }
      else if (strcmp (name, "secret") == 0)
        {
          /* Not on the executor (the keystore may block), with the configuration a job would have. */
          g_autoptr (BroBroError) error = NULL;
          g_autoptr (BroConfig) config = bro_config_service_current (h.service); /* only read; nothing runs meanwhile */
          g_autofree char *value = bro_config_service_secret (h.service, ((BroJsonValue *) op->items->pdata[0])->string, config, &error);
          record (&h, i, error ? error_json (error) : value ? bro_json_value_new_string (value) : bro_json_value_new_null ());
          bro_engine_executor_barrier (w.executor);
        }
      else if (strcmp (name, "stopFailing") == 0)
        {
          const char *secret = ((BroJsonValue *) op->items->pdata[0])->string;
          TestConfigFakes *stores[] = { f, folder };
          for (guint k = 0; k < G_N_ELEMENTS (stores); k++)
            {
              BroJsonValue *kept = bro_json_value_new_object ();
              for (guint m = 0; m < stores[k]->keystore_fails->keys->len; m++)
                if (strcmp (stores[k]->keystore_fails->keys->pdata[m], secret) != 0)
                  bro_json_value_set (kept, stores[k]->keystore_fails->keys->pdata[m], bro_json_value_ref (stores[k]->keystore_fails->items->pdata[m]));
              g_mutex_lock (&stores[k]->lock);
              bro_json_value_unref (stores[k]->keystore_fails);
              stores[k]->keystore_fails = kept;
              g_mutex_unlock (&stores[k]->lock);
            }
        }
      else if (strcmp (name, "changeOutside") == 0)
        {
          BroJsonValue *arg = op->items->pdata[0];
          TestConfigFakes *store = strcmp (bro_json_value_get_string (bro_json_value_member (arg, "store"), ""), "keystore") == 0 ? f : folder;
          const char *secret = bro_json_value_get_string (bro_json_value_member (arg, "name"), "");
          const char *value = bro_json_value_get_string (bro_json_value_member (arg, "value"), NULL);
          g_mutex_lock (&store->lock);
          if (value)
            g_hash_table_replace (store->keystore, g_strdup (secret), g_strdup (value));
          else
            g_hash_table_remove (store->keystore, secret);
          g_mutex_unlock (&store->lock);
        }
      else
        {
          ConfigOp o = { &h, i, op };
          gboolean wait = strcmp (name, "update") != 0
                          || bro_json_value_get_bool (bro_json_value_member (op->items->pdata[0], "wait"), TRUE);
          bro_engine_executor_post (w.executor, config_op_action, &o, NULL);
          bro_engine_executor_barrier (w.executor);
          g_array_append_val (started, i);
          if (wait)
            wait_for (&h, i);
          else
            {
              gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
              g_mutex_lock (&f->lock);
              while (!f->held)
                if (!g_cond_wait_until (&f->cond, &f->lock, until))
                  g_error ("the update didn't reach its write");
              g_mutex_unlock (&f->lock);
            }
        }
    }
  for (guint i = 0; i < started->len; i++)
    wait_for (&h, g_array_index (started, guint, i));
  bro_engine_executor_barrier (w.executor);

  for (guint i = 0; i < n; i++)
    {
      BroJsonValue *op = bro_json_value_at (ops, i);
      const char *name = op->kind == BRO_JSON_VALUE_STRING ? op->string : op->keys->pdata[0];
      BroJsonValue *want = bro_json_value_at (want_results, r);
      g_autoptr (BroJsonValue) normalized = NULL;
      g_autofree char *what = NULL;
      if (strcmp (name, "releaseWrites") == 0 || strcmp (name, "changeOutside") == 0 || strcmp (name, "stopFailing") == 0)
        continue;
      r++;
      if (want && !bro_json_value_member (want, "error"))
        {
          if (strcmp (name, "update") == 0 || strcmp (name, "envelope") == 0)
            normalized = norm_member (want, "doc");
          else if (strcmp (name, "current") == 0)
            normalized = norm (want);
          else if (strcmp (name, "export") == 0)
            normalized = norm_member (want, "config");
        }
      what = g_strdup_printf ("result of op %u (%s)", i, name);
      same_sorted (failures, id, what, normalized ? normalized : want, h.results[i]);
    }
  if (r != bro_json_value_length (want_results))
    bro_test_fail (failures, id, "%u results expected, %u ops", bro_json_value_length (want_results), r);

  v = bro_json_value_member (expect, "file");
  if (v->kind == BRO_JSON_VALUE_NULL)
    {
      if (f->file)
        bro_test_fail (failures, id, "file: expected none");
    }
  else if (bro_json_value_member (v, "text"))
    {
      gsize size = 0;
      const char *text = f->file ? g_bytes_get_data (f->file, &size) : "";
      g_autofree char *got = g_strndup (text, size);
      bro_test_same_string (failures, id, "file", bro_json_value_get_string (bro_json_value_member (v, "text"), ""), got);
    }
  else
    {
      gsize size = 0;
      const char *text = f->file ? g_bytes_get_data (f->file, &size) : "";
      g_autoptr (BroJsonValue) parsed = bro_json_value_parse (text, (gssize) size);
      g_autoptr (BroJsonValue) want = norm (v);
      g_autoptr (BroJsonValue) got = parsed ? norm (parsed) : NULL;
      same_sorted (failures, id, "file", want, got);
      if (f->mode != -1)
        {
          g_autoptr (BroConfig) c = NULL;
          g_autoptr (GBytes) canonical = NULL;
          g_autofree char *got_text = g_strndup (text, size);
          gsize n_canonical;
          const char *canonical_text;
          g_autofree char *want_text = NULL;
          g_autoptr (GBytes) encoded = bro_json_value_encode_canonical (v);
          gsize n_encoded;
          const char *encoded_text = g_bytes_get_data (encoded, &n_encoded);
          if (f->mode != 0600)
            bro_test_fail (failures, id, "file mode: %o", (unsigned) f->mode);
          c = bro_config_codec_decode (encoded_text, (gssize) n_encoded, NULL);
          canonical = bro_config_codec_encode (c);
          canonical_text = g_bytes_get_data (canonical, &n_canonical);
          want_text = g_strndup (canonical_text, n_canonical);
          bro_test_same_string (failures, id, "file bytes", want_text, got_text);
        }
    }

  {
    BroJsonValue *want_kv = bro_json_value_member (expect, "kv");
    g_autoptr (BroJsonValue) want = bro_json_value_member (want_kv, "config.lastGood") ? norm_member (want_kv, "config.lastGood")
                                                                                       : bro_json_value_ref (want_kv);
    g_autoptr (BroJsonValue) got_raw = bro_json_value_new_object ();
    g_autoptr (BroJsonValue) got = NULL;
    GHashTableIter it;
    gpointer k, value;
    g_hash_table_iter_init (&it, f->kv);
    while (g_hash_table_iter_next (&it, &k, &value))
      bro_json_value_set (got_raw, k, bro_json_value_ref (value));
    got = bro_json_value_member (got_raw, "config.lastGood") ? norm_member (got_raw, "config.lastGood") : bro_json_value_ref (got_raw);
    same_sorted (failures, id, "kv", want, got);
  }
  {
    g_autoptr (BroJsonValue) got = bro_json_value_new_object ();
    GHashTableIter it;
    gpointer k, value;
    g_hash_table_iter_init (&it, f->keystore);
    while (g_hash_table_iter_next (&it, &k, &value))
      bro_json_value_set (got, k, bro_json_value_new_string (value));
    same_sorted (failures, id, "keystore", bro_json_value_member (expect, "keystore"), got);
  }
  {
    g_autoptr (BroJsonValue) got = bro_json_value_new_object ();
    g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
    BroJsonValue *want = bro_json_value_member (expect, "files");
    GHashTableIter it;
    gpointer k, value;
    g_hash_table_iter_init (&it, folder->keystore);
    while (g_hash_table_iter_next (&it, &k, &value))
      bro_json_value_set (got, k, bro_json_value_new_string (value));
    same_sorted (failures, id, "files", want ? want : none, got);
  }
  {
    g_autoptr (BroJsonValue) got_events = events_json (s.events);
    g_autoptr (BroJsonValue) got_problems = problems_json (problems);
    same_sorted (failures, id, "events", bro_json_value_member (expect, "events"), got_events);
    same_sorted (failures, id, "problems", bro_json_value_member (expect, "problems"), got_problems);
  }

  bro_engine_executor_stop (w.executor);
  subscribe_harness_clear (&s);
  for (guint i = 0; i < n; i++)
    g_clear_pointer (&h.results[i], bro_json_value_unref);
  g_free (h.results);
  g_free (h.done);
  g_mutex_clear (&h.lock);
  g_cond_clear (&h.cond);
  g_object_unref (h.service);
  g_object_unref (problems);
  g_object_unref (bus);
  g_object_unref (f);
  g_object_unref (folder);
  world_clear (&w);
  return TRUE;
}

static void
test_config_service (void)
{
  bro_test_run_cases ("engine/config-service.cases.json", config_case);
}
