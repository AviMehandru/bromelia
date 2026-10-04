/* bro-store.c */
#include "bro-store.h"

G_DEFINE_INTERFACE (BroStore, bro_store, G_TYPE_OBJECT)

static void
bro_store_default_init (BroStoreInterface *iface)
{
}

BroJobRepository *
bro_store_jobs (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->jobs (self);
}

BroStepRepository *
bro_store_steps (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->steps (self);
}

BroUnitRepository *
bro_store_units (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->units (self);
}

BroCatalogRepository *
bro_store_catalog (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->catalog (self);
}

BroCheckRepository *
bro_store_checks (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->checks (self);
}

BroReplicaRepository *
bro_store_replicas (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->replicas (self);
}

BroDriveRepository *
bro_store_drives (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->drives (self);
}

BroLookupCacheRepository *
bro_store_lookup_cache (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->lookup_cache (self);
}

BroOutboxRepository *
bro_store_outbox (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->outbox (self);
}

BroKeyValueRepository *
bro_store_kv (BroStore *self)
{
  g_return_val_if_fail (BRO_IS_STORE (self), NULL);
  return BRO_STORE_GET_IFACE (self)->kv (self);
}

gboolean
bro_store_transaction (BroStore *self, BroStoreBlockFunc block, gpointer data, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_STORE (self), FALSE);
  return BRO_STORE_GET_IFACE (self)->transaction (self, block, data, error);
}

gboolean
bro_store_migrate (BroStore *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_STORE (self), FALSE);
  return BRO_STORE_GET_IFACE (self)->migrate (self, error);
}
