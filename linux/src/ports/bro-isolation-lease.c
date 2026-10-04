/* bro-isolation-lease.c */
#include "bro-isolation-lease.h"

G_DEFINE_INTERFACE (BroIsolationLease, bro_isolation_lease, G_TYPE_OBJECT)

static void
bro_isolation_lease_default_init (BroIsolationLeaseInterface *iface)
{
}

GHashTable *
bro_isolation_lease_environment (BroIsolationLease *self)
{
  g_return_val_if_fail (BRO_IS_ISOLATION_LEASE (self), NULL);
  return BRO_ISOLATION_LEASE_GET_IFACE (self)->environment (self);
}

char *
bro_isolation_lease_profile_path (BroIsolationLease *self)
{
  g_return_val_if_fail (BRO_IS_ISOLATION_LEASE (self), NULL);
  return BRO_ISOLATION_LEASE_GET_IFACE (self)->profile_path (self);
}

void
bro_isolation_lease_first_output (BroIsolationLease *self)
{
  g_return_if_fail (BRO_IS_ISOLATION_LEASE (self));
  BRO_ISOLATION_LEASE_GET_IFACE (self)->first_output (self);
}

void
bro_isolation_lease_release (BroIsolationLease *self)
{
  g_return_if_fail (BRO_IS_ISOLATION_LEASE (self));
  BRO_ISOLATION_LEASE_GET_IFACE (self)->release (self);
}
