/* test-makemkv-service.inc: shared/fixtures/engine/makemkv-service.cases.json: bro_makemkv_setup_invocation of a
 * BroMakemkvService behind a real BroConfigService (test-services.inc's fakes: a system keystore, an empty secrets
 * folder), on a real executor. Included after test-services.inc. */

#include "bro-makemkv-service.h"
#include "bro-profile-resolver.h"

/* The user's settings.conf, in memory: exists and read only. */
#define TEST_TYPE_USER_FILES (test_user_files_get_type ())
G_DECLARE_FINAL_TYPE (TestUserFiles, test_user_files, TEST, USER_FILES, GObject)

struct _TestUserFiles {
  GObject parent_instance;
  char *path;
  char *text;         /* NULL: no such file */
  BroJsonValue *fails; /* NULL: reads work */
};

static void user_files_fs_iface (BroFileSystemInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestUserFiles, test_user_files, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, user_files_fs_iface))

static gboolean
user_files_exists (BroFileSystem *fs, const char *path)
{
  TestUserFiles *f = TEST_USER_FILES (fs);
  return f->text && g_strcmp0 (path, f->path) == 0;
}

static GBytes *
user_files_read (BroFileSystem *fs, const char *path, BroBroError **error)
{
  TestUserFiles *f = TEST_USER_FILES (fs);
  if (f->fails)
    {
      *error = failure_of (f->fails);
      return NULL;
    }
  if (!user_files_exists (fs, path))
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (path));
      bro_bro_error_set (error, "fs.notFound", params);
      return NULL;
    }
  return g_bytes_new (f->text, strlen (f->text));
}

static void
user_files_fs_iface (BroFileSystemInterface *iface)
{
  iface->exists = user_files_exists;
  iface->read = user_files_read;
}

static void
test_user_files_finalize (GObject *o)
{
  TestUserFiles *f = TEST_USER_FILES (o);
  g_free (f->path);
  g_free (f->text);
  g_clear_pointer (&f->fails, bro_json_value_unref);
  G_OBJECT_CLASS (test_user_files_parent_class)->finalize (o);
}

static void test_user_files_class_init (TestUserFilesClass *k) { G_OBJECT_CLASS (k)->finalize = test_user_files_finalize; }
static void test_user_files_init (TestUserFiles *f) {}

static BroJsonValue *
optional_string (const char *s)
{
  return s ? bro_json_value_new_string (s) : bro_json_value_new_null ();
}

/* What the cases compare: the invocation as makemkv-service.cases.json describes it. */
static BroJsonValue *
invocation_json (const BroMakemkvInvocation *inv)
{
  BroJsonValue *o = bro_json_value_new_object (), *settings = bro_json_value_new_object (), *options = bro_json_value_new_object ();
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, inv->settings->settings);
  while (g_hash_table_iter_next (&it, &k, &v))
    bro_json_value_set (settings, k, bro_json_value_new_string (v));
  bro_json_value_set (o, "settings", settings);
  if (inv->settings->profile_xml)
    {
      const char *xml = inv->settings->profile_xml;
      BroJsonValue *profile = bro_json_value_new_object ();
      const char *keys[] = { "name", "selection" }, *starts[] = { "<name lang=\"eng\">", "app_DefaultSelectionString=\"" }, *ends[] = { "</name>", "\"" };
      for (guint i = 0; i < 2; i++)
        {
          const char *a = strstr (xml, starts[i]);
          const char *from = a ? a + strlen (starts[i]) : NULL;
          const char *b = from ? strstr (from, ends[i]) : NULL;
          g_autofree char *inner = b ? g_strndup (from, (gsize) (b - from)) : g_strdup ("");
          bro_json_value_set (profile, keys[i], bro_json_value_new_string (inner));
        }
      bro_json_value_set (o, "profileXml", profile);
    }
  else
    bro_json_value_set (o, "profileXml", bro_json_value_new_null ());
  const BroMakemkvOptions *opt = &inv->options;
  bro_json_value_set (options, "profilePath", optional_string (opt->profile_path));
  bro_json_value_set (options, "minLengthSeconds", opt->min_length_seconds >= 0 ? bro_json_value_new_integer (opt->min_length_seconds) : bro_json_value_new_null ());
  bro_json_value_set (options, "cacheMB", opt->cache_mb > 0 ? bro_json_value_new_integer (opt->cache_mb) : bro_json_value_new_null ());
  bro_json_value_set (options, "directIO", opt->has_direct_io ? bro_json_value_new_bool (opt->direct_io) : bro_json_value_new_null ());
  bro_json_value_set (options, "extraArguments", bro_json_value_new_string (opt->extra_arguments ? opt->extra_arguments : ""));
  bro_json_value_set (options, "scan", bro_json_value_new_bool (opt->scan));
  bro_json_value_set (o, "options", options);
  bro_json_value_set (o, "workDirectory", optional_string (inv->settings->work_directory));
  bro_json_value_set (o, "dataDir", optional_string (inv->settings->data_dir));
  bro_json_value_set (o, "stallTimeout", inv->has_stall_timeout ? bro_json_value_new_integer ((gint64) inv->stall_timeout.seconds) : bro_json_value_new_null ());
  bro_json_value_set (o, "transcript", optional_string (inv->transcript));
  return o;
}

static gboolean
makemkv_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  World w;
  TestConfigFakes *f = g_object_new (TEST_TYPE_CONFIG_FAKES, NULL);
  TestConfigFakes *folder = g_object_new (TEST_TYPE_CONFIG_FAKES, NULL);
  TestUserFiles *user = g_object_new (TEST_TYPE_USER_FILES, NULL);
  g_autoptr (GHashTable) environment = g_hash_table_new (g_str_hash, g_str_equal);
  BroJsonValue *v;
  BroBroError *error = NULL;

  world_init (&w);
  if ((v = bro_json_value_member (given, "secrets")))
    for (guint i = 0; i < v->keys->len; i++)
      g_hash_table_insert (f->keystore, g_strdup (v->keys->pdata[i]), g_strdup (bro_json_value_get_string (v->items->pdata[i], "")));
  f->keystore_fails = (v = bro_json_value_member (given, "keystoreFails")) ? bro_json_value_ref (v) : bro_json_value_new_object ();
  f->system_store = TRUE;
  folder->keystore_fails = bro_json_value_new_object ();
  folder->system_store = FALSE;
  if ((v = bro_json_value_member (given, "environment")))
    for (guint i = 0; i < v->keys->len; i++)
      g_hash_table_insert (environment, v->keys->pdata[i], (gpointer) bro_json_value_get_string (v->items->pdata[i], ""));
  BroEventBus *bus = bro_event_bus_new (BRO_CLOCK (w.clock), 1, 1024, 1024);
  BroProblemService *problems = bro_problem_service_new (bus, BRO_CLOCK (w.clock));
  BroConfigService *config = bro_config_service_new (w.executor, bus, problems, BRO_FILE_SYSTEM (f), BRO_KEYSTORE (f), BRO_KEYSTORE (folder),
                                                     BRO_KEY_VALUE_REPOSITORY (f), CONFIG_PATH, environment);

  const char *data_dir = bro_json_value_get_string (bro_json_value_member (given, "dataDir"), "/home/u/.MakeMKV");
  g_autofree char *user_settings = g_strconcat (data_dir, "/settings.conf", NULL);
  v = bro_json_value_member (given, "userSettings");
  gboolean none = v && v->kind == BRO_JSON_VALUE_NULL;
  user->path = g_strdup (user_settings);
  user->text = g_strdup (bro_json_value_get_string (v, NULL));
  if ((v = bro_json_value_member (given, "userSettingsFails")))
    user->fails = bro_json_value_ref (v);
  BroMakemkvService *service = bro_makemkv_service_new (BRO_FILE_SYSTEM (user), config, data_dir, none ? NULL : user_settings, "/home/u");

  g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (bro_json_value_member (given, "config"));
  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);
  g_autoptr (BroConfig) doc = bro_config_codec_decode (data, (gssize) size, NULL);
  BroRuleFacts facts = { "", "", "", NULL, NULL, NULL, FALSE };
  g_autoptr (BroEffectiveProfile) profile = bro_profile_resolver_resolve (doc, bro_json_value_get_string (bro_json_value_member (given, "configId"), NULL),
                                                                          &facts, NULL);
  g_autoptr (BroMakemkvInvocation) inv = bro_makemkv_setup_invocation (
      BRO_MAKEMKV_SETUP (service), doc, profile, bro_json_value_get_string (bro_json_value_member (given, "selectionOverride"), NULL),
      bro_json_value_get_string (bro_json_value_member (given, "jobDirectory"), "/data/jobs/j1"), &error);
  g_autoptr (BroJsonValue) got = NULL;
  if (inv)
    got = invocation_json (inv);
  else
    {
      BroJsonValue *e = bro_json_value_new_object ();
      got = bro_json_value_new_object ();
      bro_json_value_set (e, "code", bro_json_value_new_string (error ? error->code : "?"));
      bro_json_value_set (e, "params", error && error->params ? bro_json_value_ref (error->params) : bro_json_value_new_object ());
      bro_json_value_set (got, "error", e);
    }
  same_sorted (failures, id, "invocation", expect, got);

  g_clear_pointer (&error, bro_bro_error_free);
  bro_engine_executor_stop (w.executor);
  g_object_unref (service);
  g_object_unref (config);
  g_object_unref (problems);
  g_object_unref (bus);
  g_object_unref (user);
  g_object_unref (f);
  g_object_unref (folder);
  world_clear (&w);
  return TRUE;
}

static void
test_makemkv_service (void)
{
  bro_test_run_cases ("engine/makemkv-service.cases.json", makemkv_case);
}
