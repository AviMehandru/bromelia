/* bro-scenario-expect.h: the expectations of an expect step (decisions 238, 241, 243), one function per key, each
 * reading what the table of decision 238 gives. Without when the step first settles (decision 239); with it, it waits for
 * the condition and never moves the clock. Placeholders: {root}, {unit8} and {jobId} of the job the step names (else the
 * only job), {unit8:<name>}, {jobId:<name>}, {driveId:<drive>}. Each returns FALSE with the run's failure set. Test code,
 * not shipped. */
#pragma once

#include "bro-scenario-steps.h"

G_BEGIN_DECLS

gboolean bro_scenario_expect_check (BroScenarioRun *run, BroJsonValue *expect);
/* Waits (never moving the clock) until the job's step has started, the job is in the state (finished: it has an outcome)
 * or has the outcome, or an event of the type has been published. */
gboolean bro_scenario_expect_when (BroScenarioRun *run, BroJsonValue *when);

gboolean bro_scenario_expect_job (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_jobs (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_files (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_file (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_sums (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_record (BroScenarioRun *run, BroJsonValue *want, const char *named);
gboolean bro_scenario_expect_transcript (BroScenarioRun *run, BroJsonValue *want, const char *named);
gboolean bro_scenario_expect_debug_log (BroScenarioRun *run, BroJsonValue *want, const char *named);
gboolean bro_scenario_expect_calls (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_log (BroScenarioRun *run, BroJsonValue *want, const char *named);
gboolean bro_scenario_expect_events (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_problems (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_drives (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_db (BroScenarioRun *run, BroJsonValue *want);
gboolean bro_scenario_expect_config (BroScenarioRun *run, BroJsonValue *want);

/* A path pattern: * within one component, ** across components; a component starting with '.' is matched only by a
 * pattern component that writes the dot; a pattern ending in '/' matches only a folder written so. */
gboolean bro_scenario_expect_glob (const char *pattern, const char *path);
/* Objects: every wanted member matches (a wanted null: absent or null). Arrays: the same length, element by element.
 * Numbers by value. A wanted string with '*' in it: a glob ('*' any run of characters; owner H3b-5). Anything else:
 * equal. */
gboolean bro_scenario_expect_matches (const BroJsonValue *want, const BroJsonValue *got);
/* The placeholders in every string (NULL with the run's failure set when a name isn't known). */
BroJsonValue *bro_scenario_expect_fill (BroScenarioRun *run, BroJsonValue *v, const char *named);

G_END_DECLS
