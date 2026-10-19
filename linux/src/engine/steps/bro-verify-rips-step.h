/* bro-verify-rips-step.h: BroVerifyRipsStep: Step 10 (plan §20.1; today's verify): checks what Acquire made before
 * anything is named, hashed or moved. A kept backup first (bro_backup_check_problem: backup.checkFailed). Then each
 * title's MKV files, in plan order, against Probe's listing title of the same index (bro_matroska_probe, then
 * bro_rip_check_check; ripcheck.checking): its notes as ripcheck.note warnings, then ripcheck.checked and, with fewer
 * tracks than the disc, ripcheck.trackSubset; a file mkvmerge can't read (ripcheck.unreadable) or that doesn't match
 * (ripcheck.mismatch) is rip.checkFailed, every title is still checked, and the step fails with rip.failed or
 * rip.titlesFailed. Without mkvmerge: ripcheck.cannotCheck and no MKV is checked, as today. Skipped when Acquire left
 * nothing. Data: {checked: [the names that passed]}. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_VERIFY_RIPS_STEP (bro_verify_rips_step_get_type ())
G_DECLARE_FINAL_TYPE (BroVerifyRipsStep, bro_verify_rips_step, BRO, VERIFY_RIPS_STEP, GObject)

BroVerifyRipsStep *bro_verify_rips_step_new (void);

G_END_DECLS
