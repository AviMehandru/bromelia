/* bro-await-media-step.h: BroAwaitMediaStep: Step 1 of an automatic rip (plan §20.1): waits up to the drive's
 * automation.waitForMountSeconds for the OS to mount the disc, then goes on either way (disc.mounted /
 * disc.notMounted). Fails with disc.changed when another disc was inserted since the job was queued. Skipped when the
 * job isn't on a drive, the drive has no device or the wait is
 * 0. Data: {mountPath}. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_AWAIT_MEDIA_STEP (bro_await_media_step_get_type ())
G_DECLARE_FINAL_TYPE (BroAwaitMediaStep, bro_await_media_step, BRO, AWAIT_MEDIA_STEP, GObject)

BroAwaitMediaStep *bro_await_media_step_new (void);

G_END_DECLS
