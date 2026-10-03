/* bro-hand-brake-args.h: BroHandBrakeArgs, HandBrake steps: HandBrakeCLI with a preset, one encode per ripped MKV,
 * next to the archive (never over it). */
#pragma once

#include "bro-progress-filter.h"
#include "bro-step-definition.h"

G_BEGIN_DECLS

/* [--preset-import-file F] [--preset P] -i input -o output [more arguments]. A leading ~ in the preset file is
 * @home (nullable). */
GStrv bro_hand_brake_args_build (const BroStepDefinition *step, const char *input, const char *output, const char *home);

/* The encode's path: the step's output path (default {outputDir}/Encoded/{stem}.mkv) filled in for the file from
 * @values (char * → char *); a leading ~ is @home (nullable). */
char *bro_hand_brake_args_output (const BroStepDefinition *step, GHashTable *values, const char *home);

/* Only MKV files are encoded (not backups, ISO images or other files). */
gboolean bro_hand_brake_args_is_source (const char *path);

/* Keeps HandBrakeCLI's progress lines (Encoding: task 1 of 1, 45.12 %) at every 10 % of each task, and every other
 * line. */
gboolean bro_hand_brake_args_keep_line (BroProgressFilter *filter, const char *line);

G_END_DECLS
