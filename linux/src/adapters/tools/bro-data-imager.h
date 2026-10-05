/* bro-data-imager.h: BroDataImager: data discs to ISO images (plan §10.1; shared/fixtures/adapters/data-imager.cases.json):
 * every sector of DriveControl.open_raw, 1 MiB at a time, into a hidden file that becomes the image only once every
 * sector was read. A read error names the first byte that can't be read (other.readError); a disc with no sectors is
 * other.emptyDisc, never an empty image. Once the image has its name it is complete: a folder sync that fails after
 * that (some network shares refuse one) is a warning on the result, fs.notSynced, not an error a retry would trip
 * over. Synchronous. */
#pragma once

#include "bro-cancellation-token.h"
#include "bro-drive-control.h"
#include "bro-file-system.h"
#include "bro-data-image-copy.h"
#include "bro-run-sink.h"
#include <glib-object.h>

G_BEGIN_DECLS


#define BRO_TYPE_DATA_IMAGER (bro_data_imager_get_type ())
G_DECLARE_FINAL_TYPE (BroDataImager, bro_data_imager, BRO, DATA_IMAGER, GObject)

/* Keeps references. */
BroDataImager *bro_data_imager_new (BroDriveControl *drives, BroFileSystem *fs);

/* Copies the disc in @device to @dest_iso (which mustn't exist; its folder must); NULL and @error set (fs.alreadyExists,
 * other.readError, other.emptyDisc, job.cancelled, fs.failed, or open_raw's error). */
BroDataImageCopy *bro_data_imager_copy (BroDataImager *self, const char *device, const char *dest_iso, BroRunSink *sink,
                                        BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
