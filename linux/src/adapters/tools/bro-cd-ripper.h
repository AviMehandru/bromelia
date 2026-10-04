/* bro-cd-ripper.h: BroCdRipper: audio CDs (plan §10.1; shared/fixtures/adapters/cd-ripper.cases.json): the profile's
 * audio CD command, else cyanrip, else abcde, run in the destination; both look the album up in MusicBrainz and name
 * the files themselves. Synchronous. */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-file-system.h"
#include "bro-process-launcher.h"
#include "bro-run-sink.h"
#include "bro-tool-locator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_CD_RIPPER (bro_cd_ripper_get_type ())
G_DECLARE_FINAL_TYPE (BroCdRipper, bro_cd_ripper, BRO, CD_RIPPER, GObject)

/* Keeps references. */
BroCdRipper *bro_cd_ripper_new (BroProcessLauncher *launcher, BroToolLocator *locator, BroFileSystem *fs);

/* Rips the CD in @device into @dest (which must exist) and returns the visible items saved there, as paths, sorted.
 * @command: the profile's audio CD command ({device} is the drive), NULL or empty for none; @stall_minutes: 0 for no
 * stall timeout. NULL and @error set: other.audioNeedsRipper, job.cancelled, process.stalled, process.failed,
 * process.savedNothing, or the launcher's process.couldNotStart. */
GStrv bro_cd_ripper_rip (BroCdRipper *self, const char *device, const char *dest, const char *command, int stall_minutes, BroRunSink *sink,
                         BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
