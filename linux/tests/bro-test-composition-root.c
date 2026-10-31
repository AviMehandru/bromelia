/* bro-test-composition-root.c */
#include "bro-test-composition-root.h"
#include "bro-english-messages.h"
#include "bro-makemkv-service.h"
#include "bro-makemkv-tool.h"
#include "bro-message-text-renderer.h"
#include "bro-mkv-tool-nix.h"
#include "bro-pipeline-runner.h"
#include "bro-step-registry.h"

#include <glib/gstdio.h>
#include <string.h>

/* ---- waiting for a callback ---------------------------------------------------------------------------------------- */

TestWaiter *
test_waiter_new (void)
{
  TestWaiter *w = g_new0 (TestWaiter, 1);
  g_atomic_ref_count_init (&w->refs);
  g_mutex_init (&w->lock);
  g_cond_init (&w->cond);
  return w;
}

TestWaiter *
test_waiter_ref (TestWaiter *waiter)
{
  g_atomic_ref_count_inc (&waiter->refs);
  return waiter;
}

void
test_waiter_unref (gpointer data)
{
  TestWaiter *w = data;
  if (!w || !g_atomic_ref_count_dec (&w->refs))
    return;
  g_clear_pointer (&w->result, bro_json_value_unref);
  bro_bro_error_free (w->error);
  g_clear_object (&w->object);
  g_mutex_clear (&w->lock);
  g_cond_clear (&w->cond);
  g_free (w);
}

void
test_waiter_finish (TestWaiter *waiter, BroJsonValue *result, gpointer object, const BroBroError *error)
{
  g_mutex_lock (&waiter->lock);
  if (!waiter->finished)
    {
      waiter->finished = TRUE;
      waiter->result = result;
      waiter->object = object;
      waiter->error = error ? bro_bro_error_copy (error) : NULL;
      result = NULL;
      object = NULL;
    }
  g_cond_broadcast (&waiter->cond);
  g_mutex_unlock (&waiter->lock);
  if (result)
    bro_json_value_unref (result);
  g_clear_object (&object);
}

gboolean
test_waiter_wait (TestWaiter *waiter, double seconds)
{
  gint64 deadline = g_get_monotonic_time () + (gint64) (seconds * G_USEC_PER_SEC);
  gboolean finished;
  g_mutex_lock (&waiter->lock);
  while (!waiter->finished && g_cond_wait_until (&waiter->cond, &waiter->lock, deadline))
    ;
  finished = waiter->finished;
  g_mutex_unlock (&waiter->lock);
  return finished;
}

void
test_waiter_done (const BroBroError *error, gpointer waiter)
{
  test_waiter_finish (waiter, NULL, NULL, error);
}

/* ---- the root ------------------------------------------------------------------------------------------------------ */

static gboolean
root_crash (const char *point, gpointer data, BroBroError **error)
{
  TestCompositionRoot *root = data;
  return !root->crash || root->crash (point, root->crash_data, error);
}

static void
root_release (gpointer data)
{
  TestCompositionRoot *root = data;
  if (root->on_release)
    root->on_release (root->on_release_data);
  bro_sqlite_store_close (root->store);
}

/* Drops the graph built last (its executor stopped, or never started anything). */
static void
root_drop (TestCompositionRoot *root)
{
  g_clear_object (&root->engine);
  g_clear_object (&root->libraries);
  g_clear_object (&root->bus);
  g_clear_object (&root->executor);
  if (root->store)
    bro_sqlite_store_close (root->store);
  g_clear_object (&root->store);
}

/* A new graph on the same database, files, clock, processes and drives; the event seqs go on from the last engine's
 * (a host starts each engine above the last one's). The store is opened and migrated here, as the real root does. */
static void
root_build (TestCompositionRoot *root)
{
  gint64 first_seq = root->bus ? bro_event_bus_last_seq (root->bus) + 1 : 1;
  g_autofree char *db = g_build_filename (root->folder, "bromelia.db", NULL);
  g_autoptr (BroBroError) error = NULL;
  BroClock *clock = BRO_CLOCK (root->clock);
  BroFileSystem *fs = BRO_FILE_SYSTEM (root->fs);
  g_autoptr (BroSchedulerLimits) limits = bro_scheduler_limits_new ();
  g_autoptr (BroProblemService) problems = NULL;
  g_autoptr (BroEngineClock) engine_clock = NULL;
  g_autoptr (BroConfigService) config = NULL;
  g_autoptr (BroScheduler) scheduler = NULL;
  g_autoptr (BroMakemkvTool) makemkv = NULL;
  g_autoptr (BroMkvToolNix) matroska = NULL;
  g_autoptr (BroDriveService) drives = NULL;
  g_autoptr (BroMakemkvService) makemkv_setup = NULL;
  g_autoptr (BroStepRegistry) registry = NULL;
  g_autoptr (BroToolSet) tools = NULL;
  g_autoptr (BroMessageTextRenderer) messages = NULL;
  g_autoptr (BroMessageTextRenderer) recovery_messages = NULL;
  BroStepEnvironment *environment;
  g_autoptr (BroPipelineRunner) runner = NULL;
  g_autoptr (BroJobService) jobs = NULL;
  g_autoptr (BroRecoveryService) recovery = NULL;
  g_autoptr (BroEngineFacts) facts = NULL;
  BroStore *store;

  root_drop (root);
  root->store = bro_sqlite_store_open (db, clock, &error);
  if (!root->store || !bro_sqlite_store_migrate (root->store, &error))
    g_error ("the test root's store: %s", error ? error->code : "?");
  store = BRO_STORE (root->store);
  root->executor = bro_engine_executor_new ("bromelia-engine");
  root->bus = bro_event_bus_new (clock, first_seq, 1024, root->stream_capacity);
  engine_clock = bro_engine_clock_new (clock, root->executor);
  problems = bro_problem_service_new (root->bus, clock);
  config = bro_config_service_new (root->executor, root->bus, problems, fs, BRO_KEYSTORE (root->keystore), BRO_KEYSTORE (root->secret_files),
                                   bro_store_kv (store), TEST_ROOT_CONFIG_PATH, root->environment);
  scheduler = bro_scheduler_new (root->executor, root->bus, limits);
  root->libraries = bro_library_service_new (root->executor, root->bus, problems, scheduler, BRO_CONFIG_SOURCE (config), engine_clock, fs,
                                             bro_store_libraries (store), bro_store_units (store), bro_store_catalog (store), root_crash, root);
  makemkv = bro_makemkv_tool_new (BRO_PROCESS_LAUNCHER (root->launcher), fs, BRO_SETTINGS_ISOLATION (root->isolation),
                                  BRO_TOOL_LOCATOR (root->locator));
  drives = bro_drive_service_new (root->executor, root->bus, problems, BRO_CONFIG_SOURCE (config), engine_clock,
                                  BRO_DEVICE_MONITOR (root->drives), BRO_DRIVE_CONTROL (root->drives), BRO_MAKEMKV (makemkv),
                                  bro_store_drives (store));
  makemkv_setup = bro_makemkv_service_new (fs, config, TEST_ROOT "/home/.MakeMKV", NULL, TEST_ROOT "/home");
  registry = bro_step_registry_standard ();
  matroska = bro_mkv_tool_nix_new (BRO_PROCESS_LAUNCHER (root->launcher), fs, BRO_TOOL_LOCATOR (root->locator), TEST_ROOT_DATA_DIR "/tmp");
  tools = bro_tool_set_new (BRO_MAKEMKV (makemkv), BRO_MATROSKA (matroska));
  messages = bro_message_text_renderer_new (bro_english_messages_catalog ());
  environment = bro_step_environment_new (tools, fs, clock, BRO_DRIVE_CONTROL (root->drives), BRO_LIBRARY_PORT (root->libraries),
                                          BRO_CATALOG_QUERIES (root->libraries), BRO_CONFIG_SOURCE (config), BRO_DRIVE_LOOKUP (drives),
                                          BRO_MAKEMKV_SETUP (makemkv_setup), TEST_ROOT_DATA_DIR, BRO_MESSAGE_RENDERER (messages), "test");
  runner = bro_pipeline_runner_new (root->executor, root->bus, scheduler, store, registry, environment, engine_clock);
  bro_step_environment_free (environment);
  jobs = bro_job_service_new (root->executor, root->bus, store, scheduler, runner, engine_clock, BRO_ID_GENERATOR (root->ids), fs,
                              TEST_ROOT_DATA_DIR);
  recovery_messages = bro_message_text_renderer_new (bro_english_messages_catalog ());
  recovery = bro_recovery_service_new (root->executor, root->bus, problems, store, registry, root->libraries, BRO_CONFIG_SOURCE (config),
                                       engine_clock, fs, BRO_SETTINGS_ISOLATION (root->isolation), BRO_MESSAGE_RENDERER (recovery_messages),
                                       TEST_ROOT_DATA_DIR);
  facts = bro_engine_facts_new ("test", 4242, "macos", TEST_ROOT_DATA_DIR, root->cores);
  /* No unexpected handler to wire: the C executor's actions can't throw (decision 208). */
  root->engine = bro_local_engine_new (root->executor, root->bus, engine_clock, problems, config, scheduler, root->libraries, drives, jobs,
                                       recovery, BRO_TOOL_LOCATOR (root->locator), BRO_ENGINE_LOG (root->log), facts, root_release, root);
}

TestCompositionRoot *
test_composition_root_new (gboolean makemkvcon, const char *makemkvcon_version, gboolean mkvmerge, GHashTable *environment, int cores,
                           guint stream_capacity)
{
  TestCompositionRoot *root = g_new0 (TestCompositionRoot, 1);
  g_autoptr (GError) error = NULL;
  GHashTableIter it;
  gpointer k, v;
  root->folder = g_dir_make_tmp ("bromelia-root-XXXXXX", &error);
  g_assert_no_error (error);
  root->clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
  root->fs = g_object_new (TEST_TYPE_MEMORY_FS, NULL);
  root->fs->any_volume = bro_volume_info_new ();
  root->fs->any_volume->id = g_strdup ("vol");
  root->fs->any_volume->free_bytes = G_GINT64_CONSTANT (1000000000000);
  root->fs->any_volume->fs_type = g_strdup ("apfs");
  root->launcher = g_object_new (TEST_TYPE_SCRIPTED_LAUNCHER, NULL);
  root->launcher->files = g_object_ref (BRO_FILE_SYSTEM (root->fs));
  root->isolation = g_object_new (TEST_TYPE_RECORDING_ISOLATION, NULL);
  root->locator = g_object_new (TEST_TYPE_FIXED_LOCATOR, NULL);
  root->locator->path = makemkvcon ? g_strdup (TEST_ROOT_MAKEMKVCON) : NULL;
  root->locator->mkvmerge = mkvmerge ? g_strdup (TEST_ROOT_MKVMERGE) : NULL;
  root->locator->makemkvcon_version = g_strdup (makemkvcon_version);
  root->drives = g_object_new (TEST_TYPE_FAKE_DRIVES, NULL);
  root->keystore = test_memory_keystore_new (TRUE);
  root->secret_files = test_memory_keystore_new (FALSE);
  root->ids = g_object_new (TEST_TYPE_IDS, NULL);
  root->log = g_object_new (TEST_TYPE_RECORDING_ENGINE_LOG, NULL);
  root->environment = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (environment)
    {
      g_hash_table_iter_init (&it, environment);
      while (g_hash_table_iter_next (&it, &k, &v))
        g_hash_table_insert (root->environment, g_strdup (k), g_strdup (v));
    }
  root->cores = cores;
  root->stream_capacity = stream_capacity;
  root->stopped = TRUE;
  test_memory_fs_put_folder (root->fs, TEST_ROOT_DATA_DIR);
  root_build (root);
  return root;
}

TestWaiter *
test_composition_root_start (TestCompositionRoot *root)
{
  TestWaiter *w = test_waiter_new ();
  root->stopped = FALSE;
  bro_local_engine_start (root->engine, test_waiter_done, test_waiter_ref (w), test_waiter_unref);
  return w;
}

TestWaiter *
test_composition_root_stop (TestCompositionRoot *root)
{
  TestWaiter *w = test_waiter_new ();
  if (root->stopped)
    {
      test_waiter_finish (w, NULL, NULL, NULL);
      return w;
    }
  root->stopped = TRUE;
  bro_engine_api_stop (BRO_ENGINE_API (root->engine), test_waiter_done, test_waiter_ref (w), test_waiter_unref);
  return w;
}

gboolean
test_composition_root_restart (TestCompositionRoot *root, double seconds, BroBroError **error)
{
  g_autoptr (TestWaiter) stopped = test_composition_root_stop (root);
  g_autoptr (TestWaiter) started = NULL;
  if (!test_waiter_wait (stopped, seconds))
    return FALSE;
  if (stopped->error)
    {
      *error = bro_bro_error_copy (stopped->error);
      return FALSE;
    }
  root_build (root);
  started = test_composition_root_start (root);
  if (!test_waiter_wait (started, seconds))
    return FALSE;
  if (started->error)
    {
      *error = bro_bro_error_copy (started->error);
      return FALSE;
    }
  return TRUE;
}

static void
root_check (gpointer data)
{
  TestWaiter *w = data;
  TestCompositionRoot *root = w->object;
  w->object = NULL;
  bro_library_service_check (root->libraries, test_waiter_done, w, test_waiter_unref);
}

/* LibraryService checks every marker now, on the executor. */
static gboolean
root_check_now (TestCompositionRoot *root, double seconds)
{
  g_autoptr (TestWaiter) w = test_waiter_new ();
  TestWaiter *posted = test_waiter_ref (w);
  posted->object = (gpointer) root; /* not a reference: root_check takes it out before anything unrefs it */
  if (!bro_engine_executor_post (root->executor, root_check, posted, NULL))
    {
      posted->object = NULL;
      test_waiter_unref (posted);
      return FALSE;
    }
  return test_waiter_wait (w, seconds);
}

static char *
aside (const char *path)
{
  g_autofree char *flat = g_strdup (path);
  g_strdelimit (flat, "/", '_');
  return g_strconcat (TEST_ROOT "/.unmounted", flat, NULL);
}

gboolean
test_composition_root_unmount (TestCompositionRoot *root, const char *path, double seconds)
{
  g_autofree char *to = aside (path);
  g_assert_true (bro_file_system_rename (BRO_FILE_SYSTEM (root->fs), path, to, NULL));
  return root_check_now (root, seconds);
}

gboolean
test_composition_root_mount (TestCompositionRoot *root, const char *path, double seconds)
{
  g_autofree char *from = aside (path);
  g_assert_true (bro_file_system_rename (BRO_FILE_SYSTEM (root->fs), from, path, NULL));
  return root_check_now (root, seconds);
}

void
test_composition_root_free (TestCompositionRoot *root)
{
  if (!root)
    return;
  if (!root->stopped)
    {
      g_autoptr (TestWaiter) w = test_composition_root_stop (root);
      test_waiter_wait (w, 40);
    }
  if (root->executor)
    bro_engine_executor_stop (root->executor);
  root_drop (root);
  bro_test_remove_tree (root->folder);
  g_free (root->folder);
  g_object_unref (root->clock);
  g_object_unref (root->fs);
  g_object_unref (root->launcher);
  g_object_unref (root->isolation);
  g_object_unref (root->locator);
  g_object_unref (root->drives);
  g_object_unref (root->keystore);
  g_object_unref (root->secret_files);
  g_object_unref (root->ids);
  g_object_unref (root->log);
  g_hash_table_unref (root->environment);
  g_free (root);
}
