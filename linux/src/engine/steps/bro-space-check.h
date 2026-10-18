/* bro-space-check.h: BroSpaceCheck, room for a plan on its library's volume (plan §21; today's checkFreeSpace). */
#pragma once

#include "bro-step-context.h"

G_BEGIN_DECLS

/* Logs space.report {free, need}, then keeps @need plus the margin (bro_space_estimate_required) on the volume of
 * @library_root: FALSE and space.insufficient fails the job before anything is written. */
gboolean bro_space_check_reserve (const BroStepContext *context, const char *library_root, gint64 need, BroBroError **error);

G_END_DECLS
