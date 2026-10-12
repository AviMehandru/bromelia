/* bro-probe-step.h: BroProbeStep: Step 2 (plan §20.1): reads the disc's listing with makemkvcon (disc.op.reading), under the profile the drive
 * gets before Plan. A disc opened in a session is read again (disc.readingAgain): ReconcileStep checks it is still that
 * disc. A listing without titles fails with disc.noTitles and MakeMKV's notice (an expired key, LibreDrive required),
 * except for a backup, which goes on as today with the opened listing (disc.usingOpenedListing) or without one
 * (disc.continuingWithoutListing). Logs disc.found, the LibreDrive detail and what the run's isolation or transcript
 * reported. Data: BroProbeOutput. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PROBE_STEP (bro_probe_step_get_type ())
G_DECLARE_FINAL_TYPE (BroProbeStep, bro_probe_step, BRO, PROBE_STEP, GObject)

BroProbeStep *bro_probe_step_new (void);

G_END_DECLS
