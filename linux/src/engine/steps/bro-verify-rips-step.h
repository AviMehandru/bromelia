/* bro-verify-rips-step.h: BroVerifyRipsStep: Step 10 (plan §20.1): checks what Acquire made before anything is named,
 * hashed or moved. A kept backup first (BackupCheck), then each title's MKV files, in plan order, against Probe's
 * listing title of the same index (MkvToolNix.probe, then RipCheck.check). A file mkvmerge can't read or that doesn't
 * match fails the step, but every title is still checked. The files no title owns are sealed too, so they come last,
 * with nothing to compare them with: readable and holding a track. Without mkvmerge no MKV is checked
 * (ripcheck.cannotCheck). Skipped when Acquire left nothing. Data: {checked: [the names that passed]}. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_VERIFY_RIPS_STEP (bro_verify_rips_step_get_type ())
G_DECLARE_FINAL_TYPE (BroVerifyRipsStep, bro_verify_rips_step, BRO, VERIFY_RIPS_STEP, GObject)

BroVerifyRipsStep *bro_verify_rips_step_new (void);

G_END_DECLS
