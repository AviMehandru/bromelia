/* test-hosts.c: the hosts layer's tests: the real composition root's smoke test (decision 175). A temporary data folder
 * and home, no system keystore, no system tools (so nothing runs makemkvcon), an empty environment. Never the default
 * data folder, the system keystore or the user's home. */
#include "bro-composition-root.h"
#include "bro-test-kit.h"

#include <string.h>
#include <unistd.h>

/* An answer from the executor thread, waited for on the test's. */
typedef struct {
  GMutex lock;
  GCond cond;
  gboolean finished;
  char *error;   /* the error's code, NULL when it succeeded */
  char *version; /* system(): the facts it gave */
  char *platform;
  char *data_dir;
  gint64 pid;
  gboolean makemkv_found;
  int tools_found;
} Answer;

static void
answer_finish (Answer *a, const BroBroError *error)
{
  g_mutex_lock (&a->lock);
  a->error = error ? g_strdup (error->code) : NULL;
  a->finished = TRUE;
  g_cond_signal (&a->cond);
  g_mutex_unlock (&a->lock);
}

static void
answer_done (const BroBroError *error, gpointer data)
{
  answer_finish (data, error);
}

static void
answer_status (const BroSystemStatusDto *status, const BroBroError *error, gpointer data)
{
  Answer *a = data;
  if (status)
    {
      a->version = g_strdup (status->facts->version);
      a->platform = g_strdup (status->facts->platform);
      a->data_dir = g_strdup (status->facts->data_dir);
      a->pid = status->facts->pid;
      a->makemkv_found = status->makemkv_found;
      for (guint i = 0; i < status->tools->len; i++)
        if (((BroToolInfo *) g_ptr_array_index (status->tools, i))->path)
          a->tools_found++;
    }
  answer_finish (a, error);
}

/* TRUE once the answer came (within @seconds). */
static gboolean
answer_wait (Answer *a, double seconds)
{
  gint64 until = g_get_monotonic_time () + (gint64) (seconds * G_USEC_PER_SEC);
  gboolean finished;
  g_mutex_lock (&a->lock);
  while (!a->finished && g_cond_wait_until (&a->cond, &a->lock, until))
    ;
  finished = a->finished;
  g_mutex_unlock (&a->lock);
  return finished;
}

static void
answer_clear (Answer *a)
{
  g_free (a->error);
  g_free (a->version);
  g_free (a->platform);
  g_free (a->data_dir);
  memset (a, 0, sizeof *a);
}

static void
test_real_root_builds_starts_answers_and_stops (void)
{
  g_autoptr (GError) gerror = NULL;
  g_autofree char *root = g_dir_make_tmp ("bromelia-host-XXXXXX", &gerror);
  g_autofree char *data = g_build_filename (root, "data", NULL);
  g_autofree char *home = g_build_filename (root, "home", NULL);
  g_autofree char *db = g_build_filename (data, "bromelia.db", NULL);
  g_autofree char *secrets = g_build_filename (data, "secrets", NULL);
  g_autoptr (GHashTable) environment = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (BroBroError) error = NULL;
  BroRootOptions options = { data, NULL, home, FALSE, FALSE, environment };
  BroLocalEngine *engine;
  Answer a = { 0 };
  g_assert_no_error (gerror);
  g_mkdir_with_parents (home, 0700);

  engine = bro_composition_root_build (&options, &error);
  g_assert_null (error);
  g_assert_nonnull (engine);
  g_assert_true (g_file_test (db, G_FILE_TEST_IS_REGULAR)); /* the store is opened in the data folder */

  bro_local_engine_start (engine, answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);

  bro_engine_api_system (BRO_ENGINE_API (engine), answer_status, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, NULL);
  g_assert_cmpstr (a.version, ==, "1.0.0");
  g_assert_cmpstr (a.version, ==, BRO_COMPOSITION_ROOT_APP_VERSION);
  g_assert_cmpstr (a.platform, ==, "linux");
  g_assert_cmpstr (a.data_dir, ==, data);
  g_assert_cmpint (a.pid, ==, getpid ());
  g_assert_false (a.makemkv_found);
  g_assert_cmpint (a.tools_found, ==, 0);
  answer_clear (&a);

  bro_engine_api_stop (BRO_ENGINE_API (engine), answer_done, &a, NULL);
  g_assert_true (answer_wait (&a, 40));
  g_assert_cmpstr (a.error, ==, NULL);
  answer_clear (&a);

  /* After stop every call answers server.stopping (on this thread: the executor has stopped). */
  bro_engine_api_system (BRO_ENGINE_API (engine), answer_status, &a, NULL);
  g_assert_true (answer_wait (&a, 30));
  g_assert_cmpstr (a.error, ==, "server.stopping");
  answer_clear (&a);
  g_object_unref (engine);

  if (g_file_test (secrets, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) dir = g_dir_open (secrets, 0, NULL);
      g_assert_null (dir ? g_dir_read_name (dir) : NULL); /* no secret was written */
    }
  bro_test_remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/hosts/real-root-builds-starts-answers-and-stops", test_real_root_builds_starts_answers_and_stops);
  return g_test_run ();
}
