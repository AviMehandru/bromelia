/* bro-step-environment.h: the parts of a StepContext every job shares; the composition root builds it. data_directory:
 * the engine's data folder (a job's folder is <data>/jobs/<id>). */
#pragma once

#include "bro-catalog-queries.h"
#include "bro-clock.h"
#include "bro-config-source.h"
#include "bro-drive-control.h"
#include "bro-drive-lookup.h"
#include "bro-file-system.h"
#include "bro-library-port.h"
#include "bro-makemkv-setup.h"
#include "bro-tool-set.h"

G_BEGIN_DECLS

typedef struct {
  BroToolSet *tools;
  BroFileSystem *fs;            /* references; nullable in tests that don't use them */
  BroClock *clock;
  BroDriveControl *drives;
  BroLibraryPort *library;
  BroCatalogQueries *catalog;
  BroConfigSource *config;
  BroDriveLookup *drive_lookup;
  BroMakemkvSetup *makemkv_setup;
  char *data_directory;
} BroStepEnvironment;

/* Takes references (and copies @tools). */
BroStepEnvironment *bro_step_environment_new (const BroToolSet *tools, BroFileSystem *fs, BroClock *clock, BroDriveControl *drives,
                                              BroLibraryPort *library, BroCatalogQueries *catalog, BroConfigSource *config,
                                              BroDriveLookup *drive_lookup, BroMakemkvSetup *makemkv_setup, const char *data_directory);
void bro_step_environment_free (BroStepEnvironment *environment);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStepEnvironment, bro_step_environment_free)

G_END_DECLS
