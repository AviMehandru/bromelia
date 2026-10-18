/* bro-space-reservations.h: BroSpaceReservations: How a step keeps room on a volume for its job (plan §21), from its worker, implemented by the runner over the Scheduler's reservations (owner, 2026-10-18): two rips on one volume count each other's space. What a job keeps goes back when it finishes. */
#pragma once

#include "bro-bro-error.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SPACE_RESERVATIONS (bro_space_reservations_get_type ())
G_DECLARE_INTERFACE (BroSpaceReservations, bro_space_reservations, BRO, SPACE_RESERVATIONS, GObject)

struct _BroSpaceReservationsInterface {
  GTypeInterface parent_iface;
  gboolean (*reserve) (BroSpaceReservations *self, const char *volume, const char *path, gint64 need, gint64 free, BroBroError **error);
};

/* Keeps @need bytes on @volume for the job, replacing what it kept there. FALSE and space.insufficient {path, need, free} when @free minus
 * what other jobs keep there is less. */
gboolean bro_space_reservations_reserve (BroSpaceReservations *self, const char *volume, const char *path, gint64 need, gint64 free,
                                         BroBroError **error);

G_END_DECLS
