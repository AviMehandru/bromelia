/* bro-step-context.h: BroStepContext, everything a step may use from its worker (plan §12.2); all of it is
 * thread-safe. The job is a snapshot: its request, its plan and the outputs of the steps before this one. The
 * configuration is the one the job started with. checkpoint keeps what a resumable step saved between its attempts.
 * messages gives a message's text for what a step writes for people to read (Seal's log copy and notes; owner,
 * 2026-10-19), and app_version is the app's version for the archive record (owner, 2026-10-20). finishing is set only
 * when the runner runs the Release step at the end of a job that stopped before it (the job's outcome): the step then
 * decides whether the disc comes out. Built by the runner. */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-checkpointer.h"
#include "bro-decided.h"
#include "bro-decision-port.h"
#include "bro-job-logger.h"
#include "bro-job-snapshot.h"
#include "bro-progress-reporter.h"
#include "bro-space-reservations.h"
#include "bro-step-environment.h"

G_BEGIN_DECLS

typedef struct {
  BroJobSnapshot *job;
  BroCancellationToken *cancel;
  BroProgressReporter *progress;
  BroJobLogger *log;
  BroDecisionPort *decisions;
  BroToolSet *tools;
  BroFileSystem *fs;
  BroClock *clock;
  BroDriveControl *drives;
  BroLibraryPort *library;
  BroCatalogQueries *catalog;
  BroConfig *config;
  BroDriveLookup *drive_lookup;
  BroMakemkvSetup *makemkv_setup;
  BroSpaceReservations *space;
  BroCheckpointer *checkpoint;
  char *job_directory;
  BroMessageRenderer *messages; /* nullable in tests that don't use it */
  char *app_version;
  BroDecided *finishing; /* nullable; owned */
} BroStepContext;

/* Takes ownership of @job and the references to @progress, @log, @decisions, @space and @checkpoint; takes a reference
 * to @cancel and copies @config and the environment's parts (messages and app_version among them). finishing starts NULL. */
BroStepContext *bro_step_context_new (BroJobSnapshot *job, BroCancellationToken *cancel, BroProgressReporter *progress, BroJobLogger *log,
                                      BroDecisionPort *decisions, BroSpaceReservations *space, BroCheckpointer *checkpoint,
                                      const BroStepEnvironment *environment, const BroConfig *config, const char *job_directory);
void bro_step_context_free (BroStepContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStepContext, bro_step_context_free)

G_END_DECLS
