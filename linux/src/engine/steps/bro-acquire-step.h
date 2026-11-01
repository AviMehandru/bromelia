/* bro-acquire-step.h: BroAcquireStep: Step 8 (plan §20.1, §12.4): makes what Plan decided in the job's staging folder
 * (bro_library_port_staging_directory), holding the drive, an acquisition slot and the library (LibraryWrite). It names,
 * hashes and moves nothing.
 * - Space: the plan's need is reserved again (reservations don't outlive the engine), less what an earlier attempt kept.
 * - infoOnly: disc-info.json, the listing Probe read (bro_listing_encode; rip.discInfoWritten).
 * - mkv: the plan's titles from the source (and disc-info.json under archive.writeDiscInfo). One makemkvcon run for every
 *   title of the listing ("all"; rip.ripping), or with Plan's one-pass minimum length when a listing read with it holds
 *   exactly the titles (rip.onePassListing, then rip.onePass or rip.onePassMismatch); else title by title
 *   (rip.rippingTitle), hand-picked tracks with +sel:all. A failed title (rip.titleFailed) leaves its files under
 *   MakeMKV's names (rip.keptUnderMakemkvName) and the others are still ripped; the step then fails (rip.failed,
 *   rip.titlesFailed) with the MakeMKV notice that explains it. A run that was stopped (drive.renumbered,
 *   space.makemkvWarning, makemkv.stalled) fails the step with that reason at once.
 * - backup, backupDecrypted: the disc as a folder or an image (profile backup.format), named backup or backup.iso; an
 *   image MakeMKV made for a folder gets .iso (backup.imageForFolder), a folder made for an image loses it
 *   (backup.folderInsteadOfIso); backup.saved, or backup.failed (a stopped run: its reason; a folder without a disc
 *   structure, which counts as nothing produced: backup.checkFailed).
 * - backupThenMkv: a decrypted backup, its structure checked (backup.checkFailed), then the titles from the backup (mapped
 *   from Probe's listing to the backup's), and the backup removed unless backup.keepAfterMkv (backup.removed).
 * Read errors are counted per title (title by title) or per run (report read_errors); Release and Commit act on them. A
 * cancel removes what the stopped run wrote (rip.partialRemoved: makemkvcon leaves an unreadable file, P2-14) unless the
 * process was abandoned after KILL (process.abandoned {tool} is logged for any run whose process was: it may still be
 * writing, and JobService keeps the job's files where they are until a restart). After each title (its files synced) and after a checked backup, a checkpoint holds
 * the disc's fingerprint and what is finished: "backup" once a backup-then-MKV job has its backup (recovery resumes from
 * it by itself, plan §15.8: recovery.resumedFromBackup), else "titles". A later attempt of the step (a retry or a
 * continue) with the same disc keeps those titles (rip.continued) and removes anything else in staging
 * (rip.leftoverRemoved; owner 2026-10-18). Data: BroAcquireOutput. */
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
