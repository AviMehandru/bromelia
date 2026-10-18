/* bro-space-reservations.c */
#include "bro-space-reservations.h"

G_DEFINE_INTERFACE (BroSpaceReservations, bro_space_reservations, G_TYPE_OBJECT)

static void
bro_space_reservations_default_init (BroSpaceReservationsInterface *iface)
{
}

gboolean
bro_space_reservations_reserve (BroSpaceReservations *self, const char *volume, const char *path, gint64 need, gint64 free,
                                BroBroError **error)
{
  return BRO_SPACE_RESERVATIONS_GET_IFACE (self)->reserve (self, volume, path, need, free, error);
}
