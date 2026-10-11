/* bro-lease.c */
#include "bro-lease.h"

BroLease *
bro_lease_new (const BroId *job_id)
{
  BroLease *l = g_atomic_rc_box_new0 (BroLease);
  l->job_id = *job_id;
  l->counted_keys = g_ptr_array_new_with_free_func (g_free);
  l->counted_counts = g_array_new (FALSE, FALSE, sizeof (int));
  return l;
}

BroLease *
bro_lease_ref (BroLease *lease)
{
  return g_atomic_rc_box_acquire (lease);
}

static void
clear_lease (BroLease *l)
{
  g_ptr_array_unref (l->counted_keys);
  g_array_unref (l->counted_counts);
}

void
bro_lease_unref (BroLease *lease)
{
  if (lease)
    g_atomic_rc_box_release_full (lease, (GDestroyNotify) clear_lease);
}
