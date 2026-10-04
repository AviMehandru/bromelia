/* bro-hand-brake.h: BroHandBrake: HandBrakeCLI (plan §10.1; shared/fixtures/adapters/handbrake.cases.json): one encode
 * of a ripped MKV with the step's preset, next to the archive (never over it: the step picks the output), and the
 * presets to offer. The step's own executable is used when it sets one; otherwise the locator's. Synchronous. */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-clock.h"
#include "bro-hand-brake-run.h"
#include "bro-process-launcher.h"
#include "bro-run-sink.h"
#include "bro-step-definition.h"
#include "bro-tool-locator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_HAND_BRAKE (bro_hand_brake_get_type ())
G_DECLARE_FINAL_TYPE (BroHandBrake, bro_hand_brake, BRO, HAND_BRAKE, GObject)

/* Presets built into HandBrake 1.6 and later (any preset name works): offered when HandBrakeCLI can't list its own.
 * NULL-terminated. */
extern const char *const bro_hand_brake_built_in_presets[];

/* @home: the home folder a leading ~ stands for (the step's executable and preset file). Keeps references. */
BroHandBrake *bro_hand_brake_new (BroProcessLauncher *launcher, BroToolLocator *locator, BroClock *clock, const char *home);

/* HandBrakeCLI --preset-list's names, in its order; the built-in presets when HandBrakeCLI is missing or fails. NULL
 * and @error set (job.cancelled) when cancelled. */
GStrv bro_hand_brake_presets (BroHandBrake *self, BroCancellationToken *cancel, BroBroError **error);

/* Encodes @input to @output (whose folder must exist) in that folder. Progress goes to @sink as PROGRESS_VALUE (of
 * 10000), the lines bro_hand_brake_args_keep_line keeps as RAW. FALSE and @error set when HandBrakeCLI can't be found
 * (tool.notFoundAt, tool.missing) or started, or when cancelled (job.cancelled); otherwise *@run tells how it ended. */
gboolean bro_hand_brake_encode (BroHandBrake *self, const BroStepDefinition *step, const char *input, const char *output, BroRunSink *sink,
                                BroCancellationToken *cancel, BroHandBrakeRun *run, BroBroError **error);

G_END_DECLS
