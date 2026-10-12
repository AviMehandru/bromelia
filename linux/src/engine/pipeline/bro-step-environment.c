/* bro-step-environment.c */
#include "bro-step-environment.h"

static gpointer
ref_or_null (gpointer object)
{
  return object ? g_object_ref (object) : NULL;
}

BroStepEnvironment *
bro_step_environment_new (const BroToolSet *tools, BroFileSystem *fs, BroClock *clock, BroDriveControl *drives, BroLibraryPort *library,
                          BroCatalogQueries *catalog, BroConfigSource *config, BroDriveLookup *drive_lookup, BroMakemkvSetup *makemkv_setup,
                          const char *data_directory)
{
  BroStepEnvironment *e = g_new0 (BroStepEnvironment, 1);
  e->tools = tools ? bro_tool_set_copy (tools) : bro_tool_set_new (NULL, NULL);
  e->fs = ref_or_null (fs);
  e->clock = ref_or_null (clock);
  e->drives = ref_or_null (drives);
  e->library = ref_or_null (library);
  e->catalog = ref_or_null (catalog);
  e->config = ref_or_null (config);
  e->drive_lookup = ref_or_null (drive_lookup);
  e->makemkv_setup = ref_or_null (makemkv_setup);
  e->data_directory = g_strdup (data_directory);
  return e;
}

void
bro_step_environment_free (BroStepEnvironment *environment)
{
  if (!environment)
    return;
  bro_tool_set_free (environment->tools);
  g_clear_object (&environment->fs);
  g_clear_object (&environment->clock);
  g_clear_object (&environment->drives);
  g_clear_object (&environment->library);
  g_clear_object (&environment->catalog);
  g_clear_object (&environment->config);
  g_clear_object (&environment->drive_lookup);
  g_clear_object (&environment->makemkv_setup);
  g_free (environment->data_directory);
  g_free (environment);
}
