/* bro-hand-brake-settings.h: a HandBrake step: HandBrakeCLI (empty = found by ToolLocator), the preset and the file
 * it comes from, the output path (step tokens; never replaces a file) and more arguments. Part of
 * BroStepDefinition. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *executable;
  char *preset;
  char *preset_file;
  char *output_path;
  char *extra_arguments;
} BroHandBrakeSettings;

G_END_DECLS
