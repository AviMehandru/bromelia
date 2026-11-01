/* bro-name-step.h: BroNameStep: Step 12 (plan §20.1): decides the unit's folder in the library and every produced item's
 * name in it, and renames inside staging so staging is laid out exactly as the unit's folder will be (Seal hashes it so,
 * Commit moves it).
 * - The names: Layouts (the profile's naming) with the identity's tokens, the date tokens in local time at the job's
 *   start (bro_clock_local_time of the job's started_at, as today's apps use the rip's time); a title's hand-given name
 *   is its own template (ASCII white space trimmed from its ends). A backup is named by the file name template, inside
 *   the backup subfolder when the job is backupThenMkv (owner H3b-1). disc-info.json and the files no title owns keep their names.
 * - A folder backup's BDMV/index.bdmv (or VIDEO_TS) gives the disc's format when it differs from the identity's (a UHD
 *   disc read without a listing looks like a Blu-ray; name.formatFromBackup): the names use it and BroNameOutput carries
 *   the corrected identity for Seal's record; the backup's new name is logged as backup.renamed too (today's apps did so).
 * - Nothing is overwritten (I5): a folder of its own takes a free name ("Name (2)"; conflictPolicy skip fails with
 *   library.folderExists); a shared folder (media server layout, conflictPolicy merge) is merged item by item, so an item
 *   whose name is taken there takes a free one. Names in staging never collide either, and an item never takes another
 *   item's Acquire name.
 * - An item whose place holds a target (its own too, ASCII case ignored: Acquire's "backup" and a media server's
 *   "Backup/") first goes to a free name at staging's root; an item inside another item's target fails with
 *   name.unusable.
 * - A name that would hold no file (empty, a component starting with '.', one of Bromelia's own file names at the unit's
 *   root) fails with name.unusable, an empty folder with name.noFolder, before anything is renamed.
 * Idempotent: the decisions (folder, and per item where it is and where it goes) are saved as the "names" checkpoint
 * before the first rename, so an attempt after a crash or a retry finds each item where it is and keeps the names
 * already given, unless one is now taken in the library; a checkpoint for other items than Acquire's (Acquire ran again)
 * is ignored. Data: BroNameOutput. Skipped when Plan or Acquire hasn't run. */
#pragma once

#include "bro-step.h"

#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_NAME_STEP_NAMES_CHECKPOINT "names"

#define BRO_TYPE_NAME_STEP (bro_name_step_get_type ())
G_DECLARE_FINAL_TYPE (BroNameStep, bro_name_step, BRO, NAME_STEP, GObject)

BroNameStep *bro_name_step_new (void);

G_END_DECLS
