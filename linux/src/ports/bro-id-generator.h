/* bro-id-generator.h: BroIdGenerator: new ids for what the engine creates (jobs first): randomness is I/O, so it comes
 * through a port (owner, 2026-10-17). The adapter gives random UUIDs; the test kit counts. */
#pragma once

#include "bro-id.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ID_GENERATOR (bro_id_generator_get_type ())
G_DECLARE_INTERFACE (BroIdGenerator, bro_id_generator, BRO, ID_GENERATOR, GObject)

struct _BroIdGeneratorInterface {
  GTypeInterface parent_iface;

  BroId (*next) (BroIdGenerator *self);
};

/* An id no other call has given: a lower-case UUID. Any thread. */
BroId bro_id_generator_next (BroIdGenerator *self);

G_END_DECLS
