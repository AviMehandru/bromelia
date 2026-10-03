/* bro-rip-check.h: BroRipCheck, checks a ripped MKV against its title in the disc listing. */
#pragma once

#include "bro-duration.h"
#include "bro-mkv-probe.h"
#include "bro-rip-check-result.h"
#include "bro-title.h"

G_BEGIN_DECLS

/* Problems: no tracks, no video track when the title has one, no duration, or a duration outside
 * bro_rip_check_tolerance. Notes: more tracks than listed, or a chapter count other than the listing's (one more is
 * fine: MakeMKV may add a chapter at 00:00). */
BroRipCheckResult *bro_rip_check_check (const BroMkvProbe *probe, const BroTitle *title);

/* The difference allowed between the listed and the actual duration: 5 s or 0.5 %, whichever is larger. */
BroDuration bro_rip_check_tolerance (double expected_seconds);

G_END_DECLS
