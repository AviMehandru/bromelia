/* bro-key-value-repository.h: BroKeyValueRepository: The kv table: small JSON values. */
#pragma once

#include "bro-bro-error.h"
#include "bro-json-value.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_KEY_VALUE_REPOSITORY (bro_key_value_repository_get_type ())
G_DECLARE_INTERFACE (BroKeyValueRepository, bro_key_value_repository, BRO, KEY_VALUE_REPOSITORY, GObject)

struct _BroKeyValueRepositoryInterface {
  GTypeInterface parent_iface;

  BroJsonValue *(*get) (BroKeyValueRepository *self, const char *key, BroBroError **error);
  gboolean (*set) (BroKeyValueRepository *self, const char *key, BroJsonValue *value, BroBroError **error);
};

/* NULL when there is none. */
BroJsonValue *bro_key_value_repository_get (BroKeyValueRepository *self, const char *key, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_key_value_repository_set (BroKeyValueRepository *self, const char *key, BroJsonValue *value, BroBroError **error);

G_END_DECLS
