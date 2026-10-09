/* bro-isolation-lease.h: BroIsolationLease: One makemkvcon launch's settings: a HOME folder, or registry values until
 * makemkvcon has read them. */
#pragma once

#include "bro-bro-message.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ISOLATION_LEASE (bro_isolation_lease_get_type ())
G_DECLARE_INTERFACE (BroIsolationLease, bro_isolation_lease, BRO, ISOLATION_LEASE, GObject)

struct _BroIsolationLeaseInterface {
  GTypeInterface parent_iface;

  GHashTable *(*environment) (BroIsolationLease *self);
  char *(*profile_path) (BroIsolationLease *self);
  void (*first_output) (BroIsolationLease *self);
  BroBroMessage *(*release) (BroIsolationLease *self);
};

/* Variables to add to the process's environment (HOME). */
GHashTable *bro_isolation_lease_environment (BroIsolationLease *self);

/* The profile file for --profile, when there is one. Free with g_free. NULL when there is none. */
char *bro_isolation_lease_profile_path (BroIsolationLease *self);

/* makemkvcon has read its settings (its first output line). */
void bro_isolation_lease_first_output (BroIsolationLease *self);

/* Gives the settings back once the run has ended: why they couldn't all be cleaned up (makemkv.keyNotRemoved: the
 * registration key left in the job's folder; makemkv.registryNotRestored: the user's registry values not back yet), or
 * NULL. Free with bro_bro_message_free. */
BroBroMessage *bro_isolation_lease_release (BroIsolationLease *self);

G_END_DECLS
