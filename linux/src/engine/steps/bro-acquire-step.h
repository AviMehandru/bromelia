/* bro-acquire-step.h: BroAcquireStep: Step 8 (plan §20.1, §12.4): makes what Plan decided in the job's staging folder,
 * holding the drive, an acquisition slot and the library. It names, hashes and moves nothing. The plan's need is
 * reserved again, less what an earlier attempt kept: reservations don't outlive the engine.
 *
 * mkv: one run for every title of the listing, or with Plan's one-pass minimum length when a listing read with it holds
 * exactly the titles; else title by title. A failed title leaves its files under MakeMKV's names, the others are still
 * ripped, and the step then fails. In a run for all titles a listed file that didn't appear fails its title too:
 * MakeMKV's counts only say how many files it saved. A file the listing doesn't name goes to others. A stopped run
 * fails the step at once with its reason.
 *
 * backup: the disc as a folder or an image, named backup or backup.iso after what MakeMKV really made (it can make an
 * image for a folder, or the reverse). A folder without a disc structure counts as nothing produced. backupThenMkv rips
 * the titles from the checked backup, mapped from Probe's listing to the backup's, then removes it unless
 * backup.keepAfterMkv.
 *
 * Read errors are counted per title or per run; Release and Commit act on them. A cancel removes what the stopped run
 * wrote, since makemkvcon leaves an unreadable file, unless the process was abandoned after KILL: it may still be
 * writing, so JobService keeps the job's files where they are until a restart.
 *
 * After each title (its files synced) and after a checked backup, a checkpoint holds the disc's fingerprint and what is
 * finished: "backup" lets recovery resume by itself (plan §15.8), else "titles". A later attempt with the same disc
 * keeps those and removes anything else in staging. Data: AcquireOutput. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_ACQUIRE_STEP_TITLES_CHECKPOINT "titles"
#define BRO_ACQUIRE_STEP_BACKUP_CHECKPOINT "backup"
#define BRO_ACQUIRE_STEP_DISC_INFO_FILE "disc-info.json"

#define BRO_TYPE_ACQUIRE_STEP (bro_acquire_step_get_type ())
G_DECLARE_FINAL_TYPE (BroAcquireStep, bro_acquire_step, BRO, ACQUIRE_STEP, GObject)

BroAcquireStep *bro_acquire_step_new (void);

G_END_DECLS
