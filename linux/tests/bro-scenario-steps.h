/* bro-scenario-steps.h: BroScenarioRun, one scenario played against BroLocalEngine over TestCompositionRoot (decisions
 * 238–243): the world, the scripted tools, the jobs the steps named, and every event of one feed opened at start and
 * drained all along (reopened after each restart, the seqs going on; what a killed engine publishes after the kill never
 * happened). Each action step makes the LocalEngine call of decision 238; an action the engine refuses fails the scenario.
 * A failure reads "<id> step <n> (<kind>): <what was expected> / <what was found>". Every wait has 10 s of real time and
 * fails the scenario (never an abort); the run stops like the engine before it is freed. Test code, not shipped. */
#pragma once

#include "bro-scenario-tools.h"
#include "bro-scenario-world.h"

G_BEGIN_DECLS

/* How long any wait may take in real time. */
#define BRO_SCENARIO_PATIENCE 10.0
/* How long a stop or a restart may take (the engine's own cap is 30 s). */
#define BRO_SCENARIO_STOP_PATIENCE 40.0

typedef struct _BroScenarioRun BroScenarioRun;

struct _BroScenarioRun {
  char *id;
  BroJsonValue *scenario;
  BroScenarioWorld *world;
  BroScenarioTools *tools;
  TestCompositionRoot *root;
  GHashTable *jobs;     /* name → job id (char *) */
  GPtrArray *job_names; /* in the order the steps named them */
  GMutex lock;          /* events, cutoff, the kill and hold state */
  GCond cond;
  GPtrArray *events;    /* BroJsonValue: {seq, at, type, data} */
  gpointer drain;       /* the current feed's drainer (nullable) */
  gboolean has_cutoff;
  gint64 cutoff;        /* the seq after which the current feed's events never happened (a kill) */
  char *kill_at;
  char *killed;
  char *hold_at;
  gboolean held, released;
  int step_number;
  char *step_kind;
  char *failure;        /* "<expected> / <found>" of the step that failed */
  GMutex step_lock;     /* running, seal_wrote, renames and the faults' counts */
  char *running;        /* the step kind between its beforeStep and afterStep points (nullable) */
  gboolean seal_wrote;  /* whether Seal has written into staging in it yet */
  int renames;          /* how many renames in staging Name has made in it */
  GPtrArray *faults;    /* given.faults, each with the times it has still to fail */
};

/* Plays the scenario; FALSE with *@failure (the full text) when it fails. */
gboolean bro_scenario_run_play (const char *id, BroJsonValue *scenario, char **failure);

/* Sets the step's failure (the first one counts); always FALSE. */
gboolean bro_scenario_run_fail (BroScenarioRun *run, const char *expected, const char *found);
gboolean bro_scenario_run_failf (BroScenarioRun *run, const char *expected, const char *format, ...) G_GNUC_PRINTF (3, 4);

/* Decision 239: until the engine is quiet and every job has settled; the clock moves to a pending timer while one hasn't. */
gboolean bro_scenario_run_settle (BroScenarioRun *run);
/* Every job (JSON array), oldest first. NULL on failure. */
BroJsonValue *bro_scenario_run_all_jobs (BroScenarioRun *run);
/* The id of the job of that name; FALSE (a failure) when there is none. */
gboolean bro_scenario_run_job (BroScenarioRun *run, const char *name, BroId *id);
/* The step's job name, else the only job's; NULL (a failure) when there is no such one. */
const char *bro_scenario_run_only_job (BroScenarioRun *run);
/* The engine's DriveId of a fake drive (by its device; free with g_free); NULL (a failure) when it isn't listed. */
char *bro_scenario_run_drive_id (BroScenarioRun *run, const char *name);
/* A copy of the events so far (BroJsonValue array). */
BroJsonValue *bro_scenario_run_events (BroScenarioRun *run);
/* The name of a job id (else the id). */
const char *bro_scenario_run_name_of (BroScenarioRun *run, const char *id);

/* The engine's answers, as their DTOs' JSON; NULL (a failure: "<step kind> to succeed / error {…}") when refused. */
BroJsonValue *bro_scenario_engine_job (BroScenarioRun *run, const BroId *id);
BroJsonValue *bro_scenario_engine_job_log (BroScenarioRun *run, const BroId *id);
BroJsonValue *bro_scenario_engine_drives (BroScenarioRun *run);
BroJsonValue *bro_scenario_engine_system (BroScenarioRun *run);
BroJsonValue *bro_scenario_engine_config (BroScenarioRun *run);

/* A path the scenario gives: relative to the root, {root} filled in. */
char *bro_scenario_path (const char *relative);
/* The JSON on one line (each run of white space with a line break in it one space). */
char *bro_scenario_json_text (const BroJsonValue *v);
/* {code, params?} of an error. */
BroJsonValue *bro_scenario_error_json (const BroBroError *error);

G_END_DECLS
