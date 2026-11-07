/* bro-commit-step.h: BroCommitStep: Step 14 (plan §20.1, §15.4; §22.3 steps 4–6 through LibraryService): moves the unit
 * Seal made in the job's staging folder into the library. Nothing is hashed again: the files and the record are Seal's.
 * The intent's items are Name's, each in place since staging is laid out as the unit's folder, then Bromelia's own
 * files Seal wrote; its destination is <library>/<Name's folder>, merged item by item when Name says the folder is
 * shared.
 *
 * A unit Seal tagged goes to _Quarantine/<Name's name> [<tag>] instead, never merged. A folder of its own that another
 * job committed after Name ran takes the next free name, unless naming.conflictPolicy is skip, and never a name the
 * library has called taken. LibraryService finishes an open intent of the unit and gives a unit committed already its
 * result again, so a rerun rolls forward; the job's cancel ends its wait for a roll forward of the unit. Holds the
 * library write. Data: CommitOutput. Skipped when Plan or Seal hasn't run. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_COMMIT_STEP (bro_commit_step_get_type ())
G_DECLARE_FINAL_TYPE (BroCommitStep, bro_commit_step, BRO, COMMIT_STEP, GObject)

BroCommitStep *bro_commit_step_new (void);

G_END_DECLS
