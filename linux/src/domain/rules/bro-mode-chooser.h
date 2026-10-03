/* bro-mode-chooser.h: BroModeChooser, what a job makes of the disc in the drive. */
#pragma once

#include "bro-disc-content.h"
#include "bro-disc-flags.h"
#include "bro-disc-format.h"
#include "bro-profile.h"
#include "bro-rip-mode.h"

G_BEGIN_DECLS

/* A disc with DVD, Blu-ray or HD DVD files, or video content, gets the profile's mode: by format (mode.byFormat;
 * BRO_DISC_FORMAT_UNKNOWN: none) unless titles were chosen by hand, else mode.default. An audio CD gets audioCD and a
 * data disc dataImage when the profile's otherDiscs allow it. FALSE for a blank disc, or another disc that isn't
 * allowed (it's left alone). @profile may be sparse: missing fields take their defaults. */
gboolean bro_mode_chooser_mode (BroDiscFormat format, BroDiscFlags flags, BroDiscContent content, const BroProfile *profile,
                                gboolean chosen_by_hand, BroRipMode *out);

G_END_DECLS
