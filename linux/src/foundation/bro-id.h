/* bro-id.h: a UUID in lower-case text. Ids are made by the engine's adapters (randomness is I/O). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char value[37];
} BroId;

/* The first eight hex digits, as used in file names (bromelia-<unit8>.json). Free with g_free. */
char *bro_id_short (const BroId *id);

G_END_DECLS
