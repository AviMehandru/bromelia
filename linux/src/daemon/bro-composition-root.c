/* bro-composition-root.c */
#include "bro-composition-root.h"

#include "bro-english-messages.h"
#include "bro-file-engine-log.h"
#include "bro-home-dir-isolation.h"
#include "bro-makemkv-service.h"
#include "bro-makemkv-tool.h"
#include "bro-message-text-renderer.h"
#include "bro-mkv-tool-nix.h"
#include "bro-platform-device-monitor.h"
#include "bro-platform-drive-control.h"
#include "bro-platform-file-system.h"
#include "bro-platform-keystore.h"
#include "bro-platform-process-launcher.h"
#include "bro-platform-tool-paths.h"
#include "bro-random-id-generator.h"
#include "bro-sqlite-store.h"
#include "bro-step-registry.h"
#include "bro-system-clock.h"
#include "bro-system-tool-locator.h"

#include <unistd.h>

#define STORE_KEY "bro-composition-root-store"

/* The engine's release (on the executor thread, after it stopped): the store is closed. */
static void
release_store (gpointer store)
{
  bro_sqlite_store_close (store);
}

/* With the engine: the store goes when the engine does. */
static void
drop_store (gpointer store)
{
  bro_sqlite_store_close (store);
  g_object_unref (store);
}

static GHashTable *
secret_variables (GHashTable *environment)
{
  GHashTable *out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GHashTableIter it;
  gpointer k, v;
  if (!environment)
    return out;
  g_hash_table_iter_init (&it, environment);
  while (g_hash_table_iter_next (&it, &k, &v))
    if (g_str_has_prefix (k, "BROMELIA_SECRET_"))
      g_hash_table_insert (out, g_strdup (k), g_strdup (v));
  return out;
}

BroLocalEngine *
bro_composition_root_build (const BroRootOptions *options, BroBroError **error)
{
  const char *data = options->data_dir;
  g_autofree char *config_path = options->config_path ? g_strdup (options->config_path) : g_build_filename (data, "config.json", NULL);
  g_autofree char *secrets = g_build_filename (data, "secrets", NULL);
  g_autofree char *db = g_build_filename (data, "bromelia.db", NULL);
  g_autofree char *tmp = g_build_filename (data, "tmp", NULL);
  g_autofree char *engine_log = g_build_filename (data, "logs", "engine.log", NULL);
  g_autofree char *makemkv_folder = g_build_filename (options->home, ".MakeMKV", NULL);
  g_autofree char *user_settings = g_build_filename (makemkv_folder, "settings.conf", NULL);
  const char *path_variable = options->system_tools && options->environment ? g_hash_table_lookup (options->environment, "PATH") : NULL;
  g_auto (GStrv) search_path = path_variable ? g_strsplit (path_variable, ":", -1) : g_new0 (char *, 1);
  g_autoptr (GHashTable) configured = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  g_autoptr (GHashTable) candidates = options->system_tools ? bro_platform_tool_paths_candidates (options->home)
                                                            : g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_strfreev);
  g_autoptr (GHashTable) names = bro_platform_tool_paths_names ();
  g_autoptr (GHashTable) environment = secret_variables (options->environment);
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (BroPlatformProcessLauncher) launcher = bro_platform_process_launcher_new ();
  g_autoptr (BroPlatformKeystore) keystore = bro_platform_keystore_new (secrets, "Bromelia", options->system_keystore);
  g_autoptr (BroPlatformKeystore) secret_files = bro_platform_keystore_new (secrets, "Bromelia", FALSE);
  g_autoptr (BroSqliteStore) store = NULL;
  g_autoptr (BroSystemToolLocator) locator = NULL;
  g_autoptr (BroHomeDirIsolation) isolation = NULL;
  g_autoptr (BroMakemkvTool) makemkv = NULL;
  g_autoptr (BroMkvToolNix) matroska = NULL;
  g_autoptr (BroPlatformDeviceMonitor) monitor = NULL;
  g_autoptr (BroPlatformDriveControl) control = NULL;
  g_autoptr (BroRandomIdGenerator) ids = NULL;
  g_autoptr (BroMessageTextRenderer) messages = NULL;
  g_autoptr (BroFileEngineLog) log = NULL;
  g_autoptr (BroEngineExecutor) executor = NULL;
  g_autoptr (BroEventBus) bus = NULL;
  g_autoptr (BroEngineClock) engine_clock = NULL;
  g_autoptr (BroProblemService) problems = NULL;
  g_autoptr (BroConfigService) config = NULL;
  g_autoptr (BroSchedulerLimits) limits = bro_scheduler_limits_new ();
  g_autoptr (BroScheduler) scheduler = NULL;
  g_autoptr (BroLibraryService) libraries = NULL;
  g_autoptr (BroDriveService) drives = NULL;
  g_autoptr (BroMakemkvService) makemkv_setup = NULL;
  g_autoptr (BroStepRegistry) registry = NULL;
  g_autoptr (BroToolSet) tools = NULL;
  BroStepEnvironment *step_environment;
  g_autoptr (BroPipelineRunner) runner = NULL;
  g_autoptr (BroJobService) jobs = NULL;
  g_autoptr (BroRecoveryService) recovery = NULL;
  g_autoptr (BroEngineFacts) facts = NULL;
  BroLocalEngine *engine;
  BroFileSystem *files = BRO_FILE_SYSTEM (fs);

  if (!bro_file_system_exists (files, data) && !bro_file_system_create_directory (files, data, FALSE, error))
    return NULL;
  if (!(store = bro_sqlite_store_open (db, BRO_CLOCK (clock), error)))
    return NULL;
  if (!bro_sqlite_store_migrate (store, error))
    {
      bro_sqlite_store_close (store);
      return NULL;
    }
  locator = bro_system_tool_locator_new (files, configured, candidates, names, (const char *const *) search_path, options->home);
  isolation = bro_home_dir_isolation_new (files, BRO_HOME_LAYOUT_LINUX);
  makemkv = bro_makemkv_tool_new (BRO_PROCESS_LAUNCHER (launcher), files, BRO_SETTINGS_ISOLATION (isolation), BRO_TOOL_LOCATOR (locator));
  matroska = bro_mkv_tool_nix_new (BRO_PROCESS_LAUNCHER (launcher), files, BRO_TOOL_LOCATOR (locator), tmp);
  monitor = bro_platform_device_monitor_new (BRO_CLOCK (clock), 0);
  control = bro_platform_drive_control_new (BRO_PROCESS_LAUNCHER (launcher), BRO_CLOCK (clock));
  ids = bro_random_id_generator_new ();
  messages = bro_message_text_renderer_new (bro_english_messages_catalog ());
  log = bro_file_engine_log_new (files, BRO_CLOCK (clock), engine_log);

  /* No unexpected handler to wire: the C executor's actions can't throw (decision 208). */
  executor = bro_engine_executor_new ("bromelia-engine");
  bus = bro_event_bus_new (BRO_CLOCK (clock), 1, 1024, 1024);
  engine_clock = bro_engine_clock_new (BRO_CLOCK (clock), executor);
  problems = bro_problem_service_new (bus, BRO_CLOCK (clock));
  config = bro_config_service_new (executor, bus, problems, files, BRO_KEYSTORE (keystore), BRO_KEYSTORE (secret_files),
                                   bro_store_kv (BRO_STORE (store)), config_path, environment);
  scheduler = bro_scheduler_new (executor, bus, limits);
  libraries = bro_library_service_new (executor, bus, problems, scheduler, BRO_CONFIG_SOURCE (config), engine_clock, files,
                                       bro_store_libraries (BRO_STORE (store)), bro_store_units (BRO_STORE (store)),
                                       bro_store_catalog (BRO_STORE (store)), NULL, NULL);
  drives = bro_drive_service_new (executor, bus, problems, BRO_CONFIG_SOURCE (config), engine_clock, BRO_DEVICE_MONITOR (monitor),
                                  BRO_DRIVE_CONTROL (control), BRO_MAKEMKV (makemkv), bro_store_drives (BRO_STORE (store)));
  /* Today's paths (Bromelia.Core MakeMKV.cs): MakeMKV's folder and the settings.conf that holds its key. */
  makemkv_setup = bro_makemkv_service_new (files, config, makemkv_folder, user_settings, options->home);
  registry = bro_step_registry_standard ();
  tools = bro_tool_set_new (BRO_MAKEMKV (makemkv), BRO_MATROSKA (matroska));
  step_environment = bro_step_environment_new (tools, files, BRO_CLOCK (clock), BRO_DRIVE_CONTROL (control), BRO_LIBRARY_PORT (libraries),
                                               BRO_CATALOG_QUERIES (libraries), BRO_CONFIG_SOURCE (config), BRO_DRIVE_LOOKUP (drives),
                                               BRO_MAKEMKV_SETUP (makemkv_setup), data, BRO_MESSAGE_RENDERER (messages),
                                               BRO_COMPOSITION_ROOT_APP_VERSION);
  runner = bro_pipeline_runner_new (executor, bus, scheduler, BRO_STORE (store), registry, step_environment, engine_clock);
  bro_step_environment_free (step_environment);
  jobs = bro_job_service_new (executor, bus, BRO_STORE (store), scheduler, runner, engine_clock, BRO_ID_GENERATOR (ids), files, data);
  recovery = bro_recovery_service_new (executor, bus, problems, BRO_STORE (store), registry, libraries, BRO_CONFIG_SOURCE (config), engine_clock,
                                       files, BRO_SETTINGS_ISOLATION (isolation), BRO_MESSAGE_RENDERER (messages), data);
  facts = bro_engine_facts_new (BRO_COMPOSITION_ROOT_APP_VERSION, getpid (), "linux", data, (int) g_get_num_processors ());
  engine = bro_local_engine_new (executor, bus, engine_clock, problems, config, scheduler, libraries, drives, jobs, recovery,
                                 BRO_TOOL_LOCATOR (locator), BRO_ENGINE_LOG (log), facts, release_store, store);
  g_object_set_data_full (G_OBJECT (engine), STORE_KEY, g_steal_pointer (&store), drop_store);
  return engine;
}
