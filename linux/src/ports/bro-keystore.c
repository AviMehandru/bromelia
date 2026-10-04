/* bro-keystore.c */
#include "bro-keystore.h"

G_DEFINE_INTERFACE (BroKeystore, bro_keystore, G_TYPE_OBJECT)

static void
bro_keystore_default_init (BroKeystoreInterface *iface)
{
}

char *
bro_keystore_get (BroKeystore *self, const char *name, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_KEYSTORE (self), NULL);
  return BRO_KEYSTORE_GET_IFACE (self)->get (self, name, error);
}

gboolean
bro_keystore_set (BroKeystore *self, const char *name, const char *value, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_KEYSTORE (self), FALSE);
  return BRO_KEYSTORE_GET_IFACE (self)->set (self, name, value, error);
}

gboolean
bro_keystore_remove (BroKeystore *self, const char *name, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_KEYSTORE (self), FALSE);
  return BRO_KEYSTORE_GET_IFACE (self)->remove (self, name, error);
}
