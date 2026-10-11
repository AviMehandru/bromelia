/* bro-lease-snapshot.h: the data of a leaseChanged event (shared/schema/internal-events.json): who holds which
 * drive (by drive id), the acquisition slots held, and the jobs holding anything, per queue. */
#pragma once

#include "bro-drive-holder.h"
#include "bro-job-counts.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  GPtrArray *drives; /* of BroDriveHolder */
  int acquisitions;
  BroJobCounts jobs;
} BroLeaseSnapshot;

/* Nothing held. */
BroLeaseSnapshot *bro_lease_snapshot_new (void);
void bro_lease_snapshot_free (BroLeaseSnapshot *snapshot);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_lease_snapshot_json (const BroLeaseSnapshot *snapshot);

/* A leaseChanged event's data, typed; NULL when it isn't one. */
BroLeaseSnapshot *bro_lease_snapshot_parse (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLeaseSnapshot, bro_lease_snapshot_free)

G_END_DECLS
