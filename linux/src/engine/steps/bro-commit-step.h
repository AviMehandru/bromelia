/* bro-commit-step.h: BroCommitStep: Step 14 (plan §20.1, §15.4; §22.3 steps 4–6 through LibraryService): moves the unit
 * Seal made in the job's staging folder into the library. Nothing is hashed again: the files are Seal's (unit_files rows),
 * the record Seal's data. The intent's items are Name's, by the names they have in staging (each in place: staging is laid
 * out as the unit's folder), a folder told from Seal's files, then Bromelia's own files Seal wrote (the record, the logs,
 * the note); its destination is <library>/<Name's folder>, merged item by item when Name says the folder is shared. A
 * unit Seal tagged (READ ERRORS) goes to _Quarantine/<Name's name> [<tag>] instead (commit.quarantined), never merged,
 * and the step reports it quarantined; otherwise archive.moved. LibraryService waits while a roll forward of the unit
 * runs (the job's cancel ends that wait), finishes an open intent of the unit and gives a unit committed already its
 * result again, so a rerun rolls forward. Holds the library write. Data: CommitOutput. Skipped when Plan or Seal hasn't
 * run. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_COMMIT_STEP (bro_commit_step_get_type ())
G_DECLARE_FINAL_TYPE (BroCommitStep, bro_commit_step, BRO, COMMIT_STEP, GObject)

BroCommitStep *bro_commit_step_new (void);

G_END_DECLS
