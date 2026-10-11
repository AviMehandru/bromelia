/* bro-lease.h: what one acquire gave a step. bro_scheduler_release gives it back, except its drives: they stay with
 * the job until bro_scheduler_release_drives or bro_scheduler_finish. Reference-counted; used on the executor. */
#pragma once

#include "bro-id.h"

G_BEGIN_DECLS

typedef struct _BroLease BroLease;

struct _BroLease {
  BroId job_id;
  gint64 seq; /* leases are numbered in the order they were granted */
  /* private */
  GPtrArray *counted_keys; /* char *, parallel to counted_counts */
  GArray *counted_counts;  /* int */
  gboolean released;
};

BroLease *bro_lease_new (const BroId *job_id, gint64 seq);
BroLease *bro_lease_ref (BroLease *lease);
void bro_lease_unref (BroLease *lease);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLease, bro_lease_unref)

G_END_DECLS
