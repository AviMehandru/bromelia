/* bro-release-step.h: BroReleaseStep: Step 9 (plan §20.1): ejects the disc when the drive's automation.ejectWhenDone
 * says so, or, after a rip with read errors (a step before it counted some), automation.ejectOnFailure (drive.ejecting,
 * drive.ejected). A failed eject is logged as drive.ejectFailed (a warning) and doesn't fail the job. The runner then
 * gives back the drive and moves the job to the processing queue: the drive takes the next disc. Skipped when the job
 * isn't on a drive. Data: {ejected}. At the end of a job that stopped before it (the context's finishing), the disc
 * comes out after a failure when automation.ejectOnFailure says so, after "already archived, skipped"
 * (disc.alreadyArchived) when ejectWhenDone says so, and when the user answered eject to Decide's question; never after
 * a cancel. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_RELEASE_STEP (bro_release_step_get_type ())
G_DECLARE_FINAL_TYPE (BroReleaseStep, bro_release_step, BRO, RELEASE_STEP, GObject)

BroReleaseStep *bro_release_step_new (void);

G_END_DECLS
