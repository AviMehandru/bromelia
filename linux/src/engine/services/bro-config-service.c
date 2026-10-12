/* bro-config-service.c */
#include "bro-config-service.h"
#include "bro-config-codec.h"
#include "bro-config-validator.h"
#include "bro-message-code.h"

#include <string.h>

#define VERSION_KEY "config.version"
#define LAST_GOOD_KEY "config.lastGood"
#define FILE_MODE 0600
#define ENVIRONMENT_PREFIX "BROMELIA_SECRET_"

struct _BroConfigService {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroProblemService *problems;
  BroFileSystem *fs;
  BroKeystore *keystore;
  BroKeyValueRepository *kv;
  char *path;
  GHashTable *environment; /* variable → value */
  BroConfig *config;
  gint64 version;
  gboolean saving;
};

static void config_source_iface_init (BroConfigSourceInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroConfigService, bro_config_service, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CONFIG_SOURCE, config_source_iface_init))

/* ---- helpers ---- */

static BroConfig *
config_copy (const BroConfig *config)
{
  GPtrArray *issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  for (guint i = 0; i < config->issues->len; i++)
    g_ptr_array_add (issues, bro_issue_copy (config->issues->pdata[i]));
  return bro_config_new (bro_json_value_ref (config->document), issues);
}

static BroConfig *
decode (const BroJsonValue *doc, BroBroError **error)
{
  g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (doc);
  gsize size;
  const char *text = g_bytes_get_data (bytes, &size);
  return bro_config_codec_decode (text, (gssize) size, error);
}

static BroConfig *
defaults (void)
{
  g_autoptr (BroJsonValue) doc = bro_json_value_new_object ();
  bro_json_value_set (doc, "version", bro_json_value_new_integer (3));
  return decode (doc, NULL); /* {"version": 3} always decodes */
}

static guint
count_errors (GPtrArray *issues)
{
  guint n = 0;
  for (guint i = 0; i < issues->len; i++)
    if (((BroIssue *) g_ptr_array_index (issues, i))->severity == BRO_SEVERITY_ERROR)
      n++;
  return n;
}

static BroBroError *
error_of (BroMessageCode code, BroJsonValue *params, BroBroError *cause)
{
  return bro_bro_error_new (bro_message_code_wire (code), params, cause);
}

static BroJsonValue *
version_data (gint64 version)
{
  BroJsonValue *data = bro_json_value_new_object ();
  bro_json_value_set (data, "version", bro_json_value_new_integer (version));
  return data;
}

static void
publish_changed (BroConfigService *self)
{
  g_autoptr (BroJsonValue) data = version_data (self->version);
  bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED, data));
}

static char *
from_environment (BroConfigService *self, const char *name)
{
  g_autofree char *upper = g_ascii_strup (name, -1);
  g_autofree char *variable = NULL;
  const char *value;
  g_strdelimit (upper, ".", '_');
  variable = g_strconcat (ENVIRONMENT_PREFIX, upper, NULL);
  value = g_hash_table_lookup (self->environment, variable);
  return value && *value ? g_strdup (value) : NULL;
}

static gboolean
is_set (BroConfigService *self, const char *name)
{
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *value = bro_keystore_get (self->keystore, name, &error);
  g_autofree char *given = NULL;
  if (value)
    return TRUE;
  given = from_environment (self, name);
  return given != NULL;
}

/* The names of the document's SecretRefs ({"secret": name}), in document order, each once. */
static void
secret_names (const BroJsonValue *v, GPtrArray *names)
{
  if (v->kind == BRO_JSON_VALUE_OBJECT && v->keys->len == 1 && strcmp (g_ptr_array_index (v->keys, 0), "secret") == 0
      && ((BroJsonValue *) g_ptr_array_index (v->items, 0))->kind == BRO_JSON_VALUE_STRING)
    {
      const char *name = ((BroJsonValue *) g_ptr_array_index (v->items, 0))->string;
      if (!g_ptr_array_find_with_equal_func (names, name, g_str_equal, NULL))
        g_ptr_array_add (names, g_strdup (name));
      return;
    }
  if (v->kind == BRO_JSON_VALUE_OBJECT || v->kind == BRO_JSON_VALUE_ARRAY)
    for (guint i = 0; i < v->items->len; i++)
      secret_names (g_ptr_array_index (v->items, i), names);
}

/* ---- one asynchronous call: shared by its workers, freed after the last ---- */

typedef struct {
  BroConfigService *self; /* a reference */
  BroConfigDone done;
  BroEnvelopeDone envelope_done;
  gpointer data;
  GDestroyNotify destroy;
  BroBroError *error;      /* a failure found before any work */
  BroJsonValue *read_version;
  GBytes *bytes;
  BroJsonValue *last_good;
  BroConfig *next;         /* load, update: the document to keep */
  gint64 next_version;
  BroConfig *snapshot;     /* envelope */
  gint64 snapshot_version;
  GPtrArray *names;        /* char * */
  GPtrArray *statuses;     /* BroSecretStatus * */
  char *name;
  char *value;
} Op;

static void
op_clear (gpointer data)
{
  Op *op = data;
  if (op->destroy)
    op->destroy (op->data);
  g_object_unref (op->self);
  g_clear_pointer (&op->error, bro_bro_error_free);
  g_clear_pointer (&op->read_version, bro_json_value_unref);
  g_clear_pointer (&op->bytes, g_bytes_unref);
  g_clear_pointer (&op->last_good, bro_json_value_unref);
  g_clear_pointer (&op->next, bro_config_free);
  g_clear_pointer (&op->snapshot, bro_config_free);
  g_clear_pointer (&op->names, g_ptr_array_unref);
  g_clear_pointer (&op->statuses, g_ptr_array_unref);
  g_free (op->name);
  g_free (op->value);
}

static Op *
op_new (BroConfigService *self, BroConfigDone done, BroEnvelopeDone envelope_done, gpointer data, GDestroyNotify destroy)
{
  Op *op = g_rc_box_new0 (Op);
  op->self = g_object_ref (self);
  op->done = done;
  op->envelope_done = envelope_done;
  op->data = data;
  op->destroy = destroy;
  return op;
}

static Op *
op_ref (Op *op)
{
  return g_rc_box_acquire (op);
}

static void
op_unref (gpointer op)
{
  g_rc_box_release_full (op, op_clear);
}

static void
finish (Op *op, BroConfigEnvelope *envelope, const BroBroError *error)
{
  if (op->envelope_done)
    op->envelope_done (envelope, error, op->data);
  else
    {
      bro_config_envelope_free (envelope);
      if (op->done)
        op->done (error, op->data);
    }
}

static void
fail_action (gpointer data)
{
  Op *op = data;
  finish (op, NULL, op->error);
}

/* Reports @error from the executor later, never inside the call. */
static void
fail_later (Op *op, BroBroError *error)
{
  op->error = error;
  bro_engine_executor_post (op->self->executor, fail_action, op_ref (op), op_unref);
}

static void
work (Op *op, BroWorkerFunc func, BroWorkerDone done)
{
  bro_engine_executor_worker (op->self->executor, func, done, op_ref (op), op_unref);
}

/* ---- envelope ---- */

static gboolean
envelope_work (gpointer data, BroBroError **error)
{
  Op *op = data;
  op->statuses = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_secret_status_free);
  for (guint i = 0; i < op->names->len; i++)
    {
      const char *name = g_ptr_array_index (op->names, i);
      g_ptr_array_add (op->statuses, bro_secret_status_new (name, is_set (op->self, name)));
    }
  return TRUE;
}

static void
envelope_done (gpointer data, const BroBroError *error)
{
  Op *op = data;
  GPtrArray *issues;
  if (error)
    {
      finish (op, NULL, error);
      return;
    }
  issues = bro_config_validator_validate (op->snapshot);
  finish (op, bro_config_envelope_new (op->snapshot_version, g_steal_pointer (&op->snapshot), issues, g_steal_pointer (&op->statuses)),
          NULL);
}

static void
start_envelope (Op *op)
{
  BroConfigService *self = op->self;
  op->snapshot = config_copy (self->config);
  op->snapshot_version = self->version;
  op->names = g_ptr_array_new_with_free_func (g_free);
  secret_names (op->snapshot->document, op->names);
  work (op, envelope_work, envelope_done);
}

void
bro_config_service_envelope (BroConfigService *self, BroEnvelopeDone done, gpointer data, GDestroyNotify destroy)
{
  Op *op = op_new (self, NULL, done, data, destroy);
  start_envelope (op);
  op_unref (op);
}

/* ---- load ---- */

static gboolean
last_good_work (gpointer data, BroBroError **error)
{
  Op *op = data;
  return bro_key_value_repository_set (op->self->kv, LAST_GOOD_KEY, op->next->document, error);
}

static void
last_good_done (gpointer data, const BroBroError *error)
{
  Op *op = data;
  if (!error)
    bro_problem_service_clear (op->self->problems, BRO_MSG_CONFIG_INVALID, NULL);
  finish (op, NULL, error);
}

static gboolean
load_work (gpointer data, BroBroError **error)
{
  Op *op = data;
  BroConfigService *self = op->self;
  BroBroError *e = NULL;
  op->read_version = bro_key_value_repository_get (self->kv, VERSION_KEY, &e);
  if (!e && bro_file_system_exists (self->fs, self->path))
    op->bytes = bro_file_system_read (self->fs, self->path, &e);
  if (!e)
    op->last_good = bro_key_value_repository_get (self->kv, LAST_GOOD_KEY, &e);
  if (e)
    {
      *error = e;
      return FALSE;
    }
  return TRUE;
}

static void
load_done (gpointer data, const BroBroError *error)
{
  Op *op = data;
  BroConfigService *self = op->self;
  g_autoptr (BroConfig) decoded = NULL;
  g_autoptr (BroBroError) decode_error = NULL;
  BroProblemAction open_settings = BRO_PROBLEM_ACTION_OPEN_SETTINGS;
  g_autoptr (BroBroMessage) message = NULL;
  g_autoptr (BroProblemDto) raised = NULL;
  BroJsonValue *params;
  guint errors = 1;
  if (error)
    {
      finish (op, NULL, error);
      return;
    }
  self->version = bro_json_value_get_integer (op->read_version, 0);
  if (!op->bytes)
    {
      g_clear_pointer (&self->config, bro_config_free);
      self->config = defaults ();
      finish (op, NULL, NULL);
      return;
    }
  {
    gsize size;
    const char *text = g_bytes_get_data (op->bytes, &size);
    decoded = bro_config_codec_decode (text, (gssize) size, &decode_error);
  }
  if (decoded)
    {
      g_autoptr (GPtrArray) issues = bro_config_validator_validate (decoded);
      errors = count_errors (issues);
    }
  if (decoded && errors == 0)
    {
      g_clear_pointer (&self->config, bro_config_free);
      self->config = g_steal_pointer (&decoded);
      op->next = config_copy (self->config);
      work (op, last_good_work, last_good_done);
      return;
    }
  g_clear_pointer (&self->config, bro_config_free);
  if (op->last_good && op->last_good->kind == BRO_JSON_VALUE_OBJECT)
    self->config = decode (op->last_good, NULL);
  if (!self->config)
    self->config = defaults ();
  params = bro_json_value_new_object ();
  bro_json_value_set (params, "count", bro_json_value_new_integer (errors));
  message = bro_bro_message_new (BRO_MSG_CONFIG_INVALID, params, BRO_SEVERITY_ERROR);
  raised = bro_problem_service_raise (self->problems, message, NULL, &open_settings, 1);
  finish (op, NULL, NULL);
}

void
bro_config_service_load (BroConfigService *self, BroConfigDone done, gpointer data, GDestroyNotify destroy)
{
  Op *op = op_new (self, done, NULL, data, destroy);
  work (op, load_work, load_done);
  op_unref (op);
}

/* ---- reading ---- */

BroConfig *
bro_config_service_current (BroConfigService *self)
{
  return config_copy (self->config);
}

static BroConfig *
config_source_current (BroConfigSource *source)
{
  return bro_config_service_current (BRO_CONFIG_SERVICE (source));
}

static void
config_source_iface_init (BroConfigSourceInterface *iface)
{
  iface->current = config_source_current;
}

gint64
bro_config_service_version (BroConfigService *self)
{
  return self->version;
}

GPtrArray *
bro_config_service_validate (BroConfigService *self, const BroJsonValue *doc, BroBroError **error)
{
  g_autoptr (BroConfig) config = decode (doc, error);
  return config ? bro_config_validator_validate (config) : NULL;
}

GBytes *
bro_config_service_export (BroConfigService *self)
{
  return bro_config_codec_export_bundle (self->config);
}

/* ---- update ---- */

static BroBroError *
save_failed (const BroBroError *cause)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "reason", bro_json_value_new_string (cause->code));
  return error_of (BRO_MSG_CONFIG_SAVE_FAILED, params, bro_bro_error_copy (cause));
}

static gboolean
version_work (gpointer data, BroBroError **error)
{
  Op *op = data;
  g_autoptr (BroJsonValue) v = bro_json_value_new_integer (op->next_version);
  return bro_key_value_repository_set (op->self->kv, VERSION_KEY, v, error);
}

static gboolean
file_work (gpointer data, BroBroError **error)
{
  Op *op = data;
  BroConfigService *self = op->self;
  g_autoptr (GBytes) bytes = NULL;
  if (!bro_key_value_repository_set (self->kv, LAST_GOOD_KEY, op->next->document, error))
    return FALSE;
  bytes = bro_config_codec_encode (op->next);
  return bro_file_system_write_atomically (self->fs, self->path, bytes, FILE_MODE, error);
}

static void
file_done (gpointer data, const BroBroError *error)
{
  Op *op = data;
  BroConfigService *self = op->self;
  self->saving = FALSE;
  if (error)
    {
      g_autoptr (BroBroError) failed = save_failed (error);
      finish (op, NULL, failed);
      return;
    }
  g_clear_pointer (&self->config, bro_config_free);
  self->config = g_steal_pointer (&op->next);
  bro_problem_service_clear (self->problems, BRO_MSG_CONFIG_INVALID, NULL);
  publish_changed (self);
  start_envelope (op);
}

static void
version_done (gpointer data, const BroBroError *error)
{
  Op *op = data;
  if (error)
    {
      g_autoptr (BroBroError) failed = save_failed (error);
      op->self->saving = FALSE;
      finish (op, NULL, failed);
      return;
    }
  op->self->version = op->next_version;
  work (op, file_work, file_done);
}

void
bro_config_service_update (BroConfigService *self, const BroJsonValue *doc, gint64 expected_version, BroEnvelopeDone done,
                           gpointer data, GDestroyNotify destroy)
{
  Op *op = op_new (self, NULL, done, data, destroy);
  BroBroError *error = NULL;
  if (self->saving || expected_version != self->version)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "current", bro_json_value_new_integer (self->version));
      fail_later (op, error_of (BRO_MSG_CONFIG_VERSION_CONFLICT, params, NULL));
    }
  else if (!(op->next = decode (doc, &error)))
    fail_later (op, error);
  else
    {
      g_autoptr (GPtrArray) issues = bro_config_validator_validate (op->next);
      guint errors = count_errors (issues);
      if (errors > 0)
        {
          BroJsonValue *params = bro_json_value_new_object ();
          BroJsonValue *list = bro_json_value_new_array ();
          for (guint i = 0; i < issues->len; i++)
            bro_json_value_append (list, bro_issue_to_json (g_ptr_array_index (issues, i)));
          bro_json_value_set (params, "count", bro_json_value_new_integer (errors));
          bro_json_value_set (params, "issues", list);
          fail_later (op, error_of (BRO_MSG_CONFIG_REJECTED, params, NULL));
        }
      else
        {
          self->saving = TRUE;
          op->next_version = self->version + 1;
          work (op, version_work, version_done);
        }
    }
  op_unref (op);
}

/* ---- secrets ---- */

char *
bro_config_service_secret (BroConfigService *self, const char *name, BroBroError **error)
{
  g_autoptr (BroBroError) failure = NULL;
  char *value = bro_keystore_get (self->keystore, name, &failure);
  if (value)
    return value;
  value = from_environment (self, name);
  if (value)
    return value;
  if (failure && error)
    *error = g_steal_pointer (&failure);
  return NULL;
}

static gboolean
set_secret_work (gpointer data, BroBroError **error)
{
  Op *op = data;
  if (!*op->value)
    return bro_keystore_remove (op->self->keystore, op->name, error);
  return bro_keystore_set (op->self->keystore, op->name, op->value, error);
}

static void
set_secret_done (gpointer data, const BroBroError *error)
{
  Op *op = data;
  if (!error)
    publish_changed (op->self);
  finish (op, NULL, error);
}

void
bro_config_service_set_secret (BroConfigService *self, const char *name, const char *value, BroConfigDone done, gpointer data,
                               GDestroyNotify destroy)
{
  Op *op = op_new (self, done, NULL, data, destroy);
  op->name = g_strdup (name);
  op->value = g_strdup (value);
  work (op, set_secret_work, set_secret_done);
  op_unref (op);
}

/* ---- the object ---- */

BroConfigService *
bro_config_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroFileSystem *fs,
                        BroKeystore *keystore, BroKeyValueRepository *kv, const char *path, GHashTable *environment)
{
  BroConfigService *self = g_object_new (BRO_TYPE_CONFIG_SERVICE, NULL);
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->problems = g_object_ref (problems);
  self->fs = g_object_ref (fs);
  self->keystore = g_object_ref (keystore);
  self->kv = g_object_ref (kv);
  self->path = g_strdup (path);
  if (environment)
    {
      GHashTableIter it;
      gpointer k, v;
      g_hash_table_iter_init (&it, environment);
      while (g_hash_table_iter_next (&it, &k, &v))
        g_hash_table_insert (self->environment, g_strdup (k), g_strdup (v));
    }
  return self;
}

static void
bro_config_service_finalize (GObject *object)
{
  BroConfigService *self = BRO_CONFIG_SERVICE (object);
  g_object_unref (self->executor);
  g_object_unref (self->bus);
  g_object_unref (self->problems);
  g_object_unref (self->fs);
  g_object_unref (self->keystore);
  g_object_unref (self->kv);
  g_free (self->path);
  g_hash_table_unref (self->environment);
  bro_config_free (self->config);
  G_OBJECT_CLASS (bro_config_service_parent_class)->finalize (object);
}

static void
bro_config_service_class_init (BroConfigServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_config_service_finalize;
}

static void
bro_config_service_init (BroConfigService *self)
{
  self->environment = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->config = defaults ();
}
