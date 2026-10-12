/* bro-secret-source.h: where a secret's value can come from, in the order they are asked (config secrets.prefer). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_SECRET_SOURCE_KEYSTORE,
  BRO_SECRET_SOURCE_ENVIRONMENT,
  BRO_SECRET_SOURCE_FILE,
} BroSecretSource;

/* The wire form ("keystore"), as in shared/schema/config-3.json's Secrets. */
const char *bro_secret_source_to_wire (BroSecretSource value);
gboolean bro_secret_source_from_wire (const char *text, BroSecretSource *out);

G_END_DECLS
