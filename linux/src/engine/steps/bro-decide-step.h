/* bro-decide-step.h: BroDecideStep: Step 7 (plan §20.1), appended by Plan only when needed. It holds the plan's library
 * for writing, so while that library is offline the Scheduler keeps the job blocked (library.offline) and it goes on by
 * itself when the library is back (owner, 2026-10-18). Then: when the library was offline at Plan, the archive check
 * Plan couldn't make (BroArchiveCheck: skip ends the job, ask asks); an archived disc under
 * automation.alreadyArchived ask: decision.archivedBefore {folder, day}, answers ripAgain (logs disc.archivedBefore),
 * skip and eject (both end the job with disc.alreadyArchivedAsk; Release at the end ejects the disc for eject); the space
 * Plan didn't reserve (BroSpaceCheck). Data: {answer} (null when nothing was asked). */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

/* The id of Decide's question about a disc archived before. */
#define BRO_DECIDE_STEP_ARCHIVED_BEFORE "archivedBefore"

#define BRO_TYPE_DECIDE_STEP (bro_decide_step_get_type ())
G_DECLARE_FINAL_TYPE (BroDecideStep, bro_decide_step, BRO, DECIDE_STEP, GObject)

BroDecideStep *bro_decide_step_new (void);

G_END_DECLS
