/* bro-isolation-lease.h: BroIsolationLease: One makemkvcon launch's settings: a HOME folder, or registry values until
 * makemkvcon has read them. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ISOLATION_LEASE (bro_isolation_lease_get_type ())
G_DECLARE_INTERFACE (BroIsolationLease, bro_isolation_lease, BRO, ISOLATION_LEASE, GObject)

struct _BroIsolationLeaseInterface {
  GTypeInterface parent_iface;

  GHashTable *(*environment) (BroIsolationLease *self);
  char *(*profile_path) (BroIsolationLease *self);
  void (*first_output) (BroIsolationLease *self);
  void (*release) (BroIsolationLease *self);
};

/* Variables to add to the process's environment (HOME). */
GHashTable *bro_isolation_lease_environment (BroIsolationLease *self);

/* The profile file for --profile, when there is one. Free with g_free. NULL when there is none. */
char *bro_isolation_lease_profile_path (BroIsolationLease *self);

/* makemkvcon has read its settings (its first output line). */
void bro_isolation_lease_first_output (BroIsolationLease *self);

void bro_isolation_lease_release (BroIsolationLease *self);

G_END_DECLS
