/* bro-name-step.h: BroNameStep: Step 12 (plan §20.1): decides the unit's folder in the library and every produced
 * item's name in it, and renames inside staging so staging is laid out exactly as the unit's folder will be: Seal
 * hashes it so, Commit moves it.
 *
 * Names come from Layouts with the identity's tokens, date tokens in local time at the job's start; a title's
 * hand-given name is its own template. disc-info.json and the files no title owns keep their names. A folder backup's
 * BDMV/index.bdmv or VIDEO_TS can correct the disc's format (a UHD disc read without a listing looks like a Blu-ray):
 * NameOutput then carries the corrected identity for Seal's record.
 *
 * Nothing is overwritten: a folder of its own takes a free name ("Name (2)"), a shared folder is merged item by item,
 * and an item whose name is taken takes a free one. An item whose place holds a target, ASCII case ignored (Acquire's
 * "backup" and a media server's "Backup/"), first goes to a free name at staging's root. A name that would hold no file
 * (empty, a component starting with '.', one of Bromelia's own file names at the unit's root) fails before anything is
 * renamed.
 *
 * Idempotent: the decisions are saved as the "names" checkpoint before the first rename, so an attempt after a crash or
 * a retry finds each item where it is and keeps the names given, unless one is now taken in the library. A checkpoint
 * for other items than Acquire's (Acquire ran again) is ignored. Data: NameOutput. Skipped when Plan or Acquire hasn't
 * run. */
#pragma once

#include "bro-step.h"

#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_NAME_STEP_NAMES_CHECKPOINT "names"

#define BRO_TYPE_NAME_STEP (bro_name_step_get_type ())
G_DECLARE_FINAL_TYPE (BroNameStep, bro_name_step, BRO, NAME_STEP, GObject)

BroNameStep *bro_name_step_new (void);

G_END_DECLS
