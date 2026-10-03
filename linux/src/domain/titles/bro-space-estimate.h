/* bro-space-estimate.h: BroSpaceEstimate, how much space a rip needs (listing sizes are estimates). */
#pragma once

#include "bro-bytes.h"
#include "bro-listing.h"

G_BEGIN_DECLS

/* @bytes plus a margin: 2 % or 256 MiB, whichever is larger. */
BroBytes bro_space_estimate_required (BroBytes bytes);

/* The chosen titles (BroTitle *), plus a second copy of the titles with hand-picked tracks (@hand_picked, int)
 * and of the "play all" title (@split_title, -1: none), split into episodes. */
BroBytes bro_space_estimate_for_plan (GPtrArray *titles, GArray *hand_picked, int split_title);

G_END_DECLS
