/* bro-hand-brake-run.h: BroHandBrakeRun: what an encode came to: HandBrakeCLI's exit, and whether the step's timeout
 * stopped it. */
#pragma once

#include "bro-process-exit.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroProcessExit exit;
  gboolean timed_out;
} BroHandBrakeRun;

G_END_DECLS
