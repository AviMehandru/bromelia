/* bro-scenario-tools.h: BroScenarioTools, the scripted tools (decision 236): a BroProcessLauncher that plays the world's
 * tool scripts (shared/scenarios/README.md "Tool scripts") instead of starting anything. The tool is the executable's
 * name. makemkvcon's drive scan (info disc:9999) is answered from the drive world; a makemkvcon backup whose destination
 * exists is refused as the real one refuses it; otherwise the first rule whose when matches writes its files, then prints
 * its lines at once. It keeps the ProcessLauncher contract the way the platform launchers do, on the TestManualClock: a
 * rule's silence, the stall watchdog (the spec's stall timeout after the last line), the stop escalation (TERM first, or
 * INT first: then TERM, then KILL, 5 s apart; a signal the script ignores does nothing; makemkvcon ignores SIGINT), a
 * process that outlives SIGKILL abandoned 30 s later, and the transcript (the platform launchers' header, every line and
 * the ending). Every launch but the drive scans is recorded (bro_scenario_tools_calls), and each one inside a step
 * reports the crash point inStep:<kind>:<n> (the step's n-th process) to the crash hook once its rule's files are written,
 * before any line. Like the real makemkvcon, the line saved of a run that wrote titles reports how many it wrote
 * (lines.yaml's says 1; owner question H3a-1). Thread-safe. Test code, not shipped. */
#pragma once

#include "bro-process-launcher.h"
#include "bro-scenario-world.h"

G_BEGIN_DECLS

#define BRO_TYPE_SCENARIO_TOOLS (bro_scenario_tools_get_type ())
G_DECLARE_FINAL_TYPE (BroScenarioTools, bro_scenario_tools, BRO, SCENARIO_TOOLS, GObject)

/* A crash point a process start reaches: FALSE (with @error) makes the start fail (a kill). */
typedef gboolean (*BroScenarioCrashFunc) (const char *point, gpointer data, BroBroError **error);

/* Keeps @world (borrowed: it outlives the tools), references @clock and @fs. */
BroScenarioTools *bro_scenario_tools_new (BroScenarioWorld *world, TestManualClock *clock, TestMemoryFs *fs);
/* The inStep:<kind>:<n> hook (nullable), set before the first start. */
void bro_scenario_tools_set_crash (BroScenarioTools *self, BroScenarioCrashFunc crash, gpointer data);
/* A step of this kind begins (its beforeStep point; NULL: none runs): the processes started from now on are its. */
void bro_scenario_tools_step_begins (BroScenarioTools *self, const char *kind);
/* Every launch but the drive scans, as "<tool> <arguments joined by spaces>", of one tool (char *; free the array). */
GPtrArray *bro_scenario_tools_calls (BroScenarioTools *self, const char *tool);
/* The processes that haven't ended and whose lines have all been read: each one parks the step's worker that waits
 * for it (it ends only when the clock moves or it is stopped). */
int bro_scenario_tools_parked (BroScenarioTools *self);
/* A kill: every process ends at once, without another line or transcript line. */
void bro_scenario_tools_end_all (BroScenarioTools *self);

G_END_DECLS
