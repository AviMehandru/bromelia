/* bro-key-value-repository.c */
#include "bro-key-value-repository.h"

G_DEFINE_INTERFACE (BroKeyValueRepository, bro_key_value_repository, G_TYPE_OBJECT)

static void
bro_key_value_repository_default_init (BroKeyValueRepositoryInterface *iface)
{
}

BroJsonValue *
bro_key_value_repository_get (BroKeyValueRepository *self, const char *key, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_KEY_VALUE_REPOSITORY (self), NULL);
  return BRO_KEY_VALUE_REPOSITORY_GET_IFACE (self)->get (self, key, error);
}

gboolean
bro_key_value_repository_set (BroKeyValueRepository *self, const char *key, BroJsonValue *value, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_KEY_VALUE_REPOSITORY (self), FALSE);
  return BRO_KEY_VALUE_REPOSITORY_GET_IFACE (self)->set (self, key, value, error);
}
