/* bro-random-id-generator.h: BroRandomIdGenerator: random (version 4) UUIDs in lower-case text. */
#pragma once

#include "bro-id-generator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_RANDOM_ID_GENERATOR (bro_random_id_generator_get_type ())
G_DECLARE_FINAL_TYPE (BroRandomIdGenerator, bro_random_id_generator, BRO, RANDOM_ID_GENERATOR, GObject)

BroRandomIdGenerator *bro_random_id_generator_new (void);

/* g_uuid_string_random. */
BroId bro_random_id_generator_next (BroRandomIdGenerator *self);

G_END_DECLS
