/* bro-resource-set.h: what a step holds while it runs (plan §21): drives, an acquisition slot, library writes,
 * CPU slots and I/O slots per volume. MakeMKV launches are taken inside RegistrySwapIsolation, not here. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  GStrv drives;         /* DriveIds; NULL-terminated, may be NULL */
  gboolean acquisition;
  GStrv library_writes; /* library ids */
  int cpu;
  GStrv io;             /* volume ids */
} BroResourceSet;

BroResourceSet *bro_resource_set_new (void);
void bro_resource_set_free (BroResourceSet *set);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroResourceSet, bro_resource_set_free)

G_END_DECLS
