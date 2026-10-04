/* test-keystore.inc: BroPlatformKeystore, included by test-adapters.c: shared/fixtures/adapters/keystore.cases.json
 * against the Secret Service (where one answers) and against the fallback files. */

#ifdef BRO_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

static gboolean keystore_system_store;

/* A step's value: the text, or {repeat, times}. NULL for JSON null. Free with g_free. */
static char *
keystore_text (const BroJsonValue *v)
{
  GString *s;
  if (!v || v->kind == BRO_JSON_VALUE_NULL)
    return NULL;
  if (v->kind == BRO_JSON_VALUE_STRING)
    return g_strdup (v->string);
  s = g_string_new (NULL);
  for (gint64 i = bro_json_value_get_integer (bro_json_value_member (v, "times"), 0); i > 0; i--)
    g_string_append (s, bro_json_value_get_string (bro_json_value_member (v, "repeat"), ""));
  return g_string_free (s, FALSE);
}

static char *
keystore_scratch (void)
{
  g_autofree char *uuid = g_uuid_string_random ();
  g_autofree char *base = g_strdup_printf ("BromeliaTest-%s", uuid);
  return g_build_filename (scratch, base, NULL);
}

static gboolean
keystore_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autofree char *dir = keystore_scratch ();
  g_autofree char *service = g_path_get_basename (dir);
  g_autoptr (BroPlatformKeystore) keystore = bro_platform_keystore_new (dir, service, keystore_system_store);
  BroJsonValue *steps = bro_json_value_member (given, "steps"), *results = bro_json_value_member (expect, "results");
  for (guint i = 0; i < bro_json_value_length (steps); i++)
    {
      BroJsonValue *step = bro_json_value_at (steps, i), *want = bro_json_value_at (results, i);
      const char *op = bro_json_value_get_string (bro_json_value_member (step, "op"), "");
      const char *name = bro_json_value_get_string (bro_json_value_member (step, "name"), "");
      const char *want_error = bro_json_value_get_string (bro_json_value_member (want, "error"), NULL);
      g_autofree char *what = g_strdup_printf ("step %u", i);
      g_autofree char *got = NULL, *value = keystore_text (bro_json_value_member (step, "value"));
      g_autoptr (BroBroError) error = NULL;
      gboolean ok = TRUE;
      if (g_str_equal (op, "get"))
        got = bro_keystore_get (BRO_KEYSTORE (keystore), name, &error);
      else if (g_str_equal (op, "set"))
        ok = bro_keystore_set (BRO_KEYSTORE (keystore), name, value, &error);
      else
        ok = bro_keystore_remove (BRO_KEYSTORE (keystore), name, &error);
      if (error || !ok)
        {
          bro_test_same_string (failures, id, what, want_error, error ? error->code : "(FALSE without an error)");
          if (error)
            bro_test_same_string (failures, id, "name", name, bro_json_value_get_string (bro_json_value_member (error->params, "name"), NULL));
        }
      else
        {
          g_autofree char *want_text = want_error ? NULL : keystore_text (want);
          bro_test_same_string (failures, id, what, want_error ? want_error : want_text, got);
        }
    }
  for (guint i = 0; i < bro_json_value_length (steps); i++)
    bro_keystore_remove (BRO_KEYSTORE (keystore), bro_json_value_get_string (bro_json_value_member (bro_json_value_at (steps, i), "name"), ""), NULL);
  remove_tree (dir);
  return TRUE;
}

/* No session bus: the files, decided at the first call. In a subprocess of its own: GIO keeps the session bus once
 * any test has opened it. */
static void
test_keystore_without_secret_service (void)
{
  if (g_test_subprocess ())
    {
      g_autofree char *dir = keystore_scratch ();
      g_autoptr (BroPlatformKeystore) keystore = NULL;
      g_autoptr (BroBroError) error = NULL;
      g_autofree char *got = NULL, *file = g_build_filename (dir, "metadata.tmdb", NULL);
      g_setenv ("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/bromelia-session-bus", TRUE);
      keystore = bro_platform_keystore_new (dir, NULL, TRUE);
      g_assert_cmpstr (bro_platform_keystore_backend (keystore), ==, "file");
      g_assert_true (bro_platform_keystore_set (keystore, "metadata.tmdb", "abc123", &error));
      g_assert_true (g_file_test (file, G_FILE_TEST_IS_REGULAR));
      got = bro_platform_keystore_get (keystore, "metadata.tmdb", &error);
      g_assert_cmpstr (got, ==, "abc123");
      remove_tree (dir);
      return;
    }
  g_test_trap_subprocess (NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
  g_test_trap_assert_passed ();
}

static void
test_keystore_files (void)
{
  g_autofree char *dir = keystore_scratch ();
  g_autofree char *file = g_build_filename (dir, "metadata.tmdb", NULL);
  g_autoptr (BroPlatformKeystore) keystore = bro_platform_keystore_new (dir, NULL, FALSE);
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *text = NULL;
  GDir *d;
  struct stat st;
  keystore_system_store = FALSE;
  bro_test_run_cases ("adapters/keystore.cases.json", keystore_case);

  g_assert_cmpstr (bro_platform_keystore_backend (keystore), ==, "file");
  g_assert_true (bro_platform_keystore_set (keystore, "metadata.tmdb", "abc123", &error));
  g_assert_true (bro_platform_keystore_set (keystore, "metadata.tmdb", "def456", &error));
  d = g_dir_open (dir, 0, NULL);
  g_assert_cmpstr (g_dir_read_name (d), ==, "metadata.tmdb"); /* one file per secret, no temporary left */
  g_assert_null (g_dir_read_name (d));
  g_dir_close (d);
  g_assert_true (g_file_get_contents (file, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "def456");
  g_assert_cmpint (stat (file, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_assert_cmpint (stat (dir, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);
  remove_tree (dir);
}

#ifdef BRO_HAVE_LIBSECRET
/* Whether the default collection is unlocked, asked of libsecret directly; -1 when there is no Secret Service. */
static int
keystore_default_unlocked (void)
{
  g_autoptr (SecretService) service = secret_service_get_sync (SECRET_SERVICE_NONE, NULL, NULL);
  g_autoptr (SecretCollection) collection = NULL;
  if (!service)
    return -1;
  collection = secret_collection_for_alias_sync (service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, NULL, NULL);
  return collection && !secret_collection_get_locked (collection);
}

static const SecretSchema keystore_test_schema = { "app.bromelia.Bromelia.Secret",
                                                    SECRET_SCHEMA_NONE,
                                                    { { "service", SECRET_SCHEMA_ATTRIBUTE_STRING }, { "name", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } } };
#endif

/* The Secret Service where one answers (the Ubuntu VM runs this inside dbus-run-session with an unlocked
 * gnome-keyring). A locked keyring fails rather than moving the secret to a file. */
static void
test_keystore_secret_service (void)
{
#ifdef BRO_HAVE_LIBSECRET
  g_autofree char *dir = keystore_scratch ();
  g_autofree char *service = g_path_get_basename (dir);
  g_autoptr (BroPlatformKeystore) keystore = bro_platform_keystore_new (dir, service, TRUE);
  g_autoptr (BroBroError) error = NULL;
  int unlocked = keystore_default_unlocked ();
  if (g_str_equal (bro_platform_keystore_backend (keystore), "file"))
    {
      g_assert_cmpint (unlocked, ==, -1);
      g_test_skip ("no Secret Service here");
      return;
    }
  if (unlocked != 1)
    {
      g_assert_false (bro_platform_keystore_set (keystore, "metadata.tmdb", "abc123", &error));
      g_assert_cmpstr (error->code, ==, "keystore.failed");
      g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS));
      g_test_message ("the keyring is locked: %s", bro_json_value_get_string (bro_json_value_member (error->params, "reason"), ""));
      return;
    }
  g_test_message ("the Secret Service, default collection unlocked");
  keystore_system_store = TRUE;
  bro_test_run_cases ("adapters/keystore.cases.json", keystore_case);
  {
    /* The item is where libsecret itself finds it, and nothing is written to the folder. */
    char *found;
    g_assert_true (bro_platform_keystore_set (keystore, "metadata.tmdb", "abc123", &error));
    found = secret_password_lookup_sync (&keystore_test_schema, NULL, NULL, "service", service, "name", "metadata.tmdb", NULL);
    g_assert_cmpstr (found, ==, "abc123");
    secret_password_free (found);
    g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS));
    g_assert_true (bro_platform_keystore_remove (keystore, "metadata.tmdb", &error));
    found = secret_password_lookup_sync (&keystore_test_schema, NULL, NULL, "service", service, "name", "metadata.tmdb", NULL);
    g_assert_null (found);
  }
  /* Locked: every call fails at once, nothing prompts (nobody may be there to answer), nothing goes to a file. Only in
   * a throw-away keyring (BROMELIA_TEST_PRIVATE_KEYRING): there's no unlocking it again without a prompt. */
  if (g_getenv ("BROMELIA_TEST_PRIVATE_KEYRING"))
    {
      g_autoptr (SecretService) secrets = secret_service_get_sync (SECRET_SERVICE_NONE, NULL, NULL);
      g_autoptr (SecretCollection) collection = secret_collection_for_alias_sync (secrets, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, NULL, NULL);
      GList *objects = g_list_append (NULL, collection);
      gint64 t0 = g_get_monotonic_time ();
      g_autofree char *got = NULL;
      g_assert_true (bro_platform_keystore_set (keystore, "metadata.tmdb", "abc123", &error));
      g_assert_cmpint (secret_service_lock_sync (secrets, objects, NULL, NULL, NULL), ==, 1);
      g_list_free (objects);
      g_assert_true (secret_collection_get_locked (collection));
      got = bro_platform_keystore_get (keystore, "metadata.tmdb", &error);
      g_assert_null (got);
      g_assert_cmpstr (error->code, ==, "keystore.failed");
      g_assert_cmpstr (bro_json_value_get_string (bro_json_value_member (error->params, "reason"), ""), ==, "the keyring is locked");
      g_clear_pointer (&error, bro_bro_error_free);
      g_assert_false (bro_platform_keystore_set (keystore, "metadata.tmdb", "def456", &error));
      g_assert_cmpstr (error->code, ==, "keystore.failed");
      g_clear_pointer (&error, bro_bro_error_free);
      g_assert_false (bro_platform_keystore_remove (keystore, "metadata.tmdb", &error));
      g_assert_cmpstr (error->code, ==, "keystore.failed");
      g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS));
      g_assert_cmpfloat (since (t0), <, 5);
      g_test_message ("a locked keyring fails at once");
    }
#else
  g_test_skip ("built without libsecret");
#endif
}
