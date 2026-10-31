/* bro-snapshot-dto.h: BroSnapshotDto: the data of a snapshot event (events.json Snapshot), never published on the bus:
 * the system, the drives, every unfinished job and the last 50 finished ones, the queues as the Scheduler orders them,
 * the problems, the libraries and the configuration's version. Sessions are empty until SessionService. */
#pragma once

#include "bro-queue-orders.h"
#include "bro-system-status-dto.h"

G_BEGIN_DECLS

typedef struct {
  BroSystemStatusDto *system;
  GPtrArray *drives;    /* BroDriveDto * */
  GPtrArray *jobs;      /* BroJobDto * */
  BroQueueOrders *queues;
  GPtrArray *problems;  /* BroProblemDto * */
  GPtrArray *libraries; /* BroLibraryDto * */
  gint64 config_version;
} BroSnapshotDto;

/* Empty lists and queues, no system yet. */
BroSnapshotDto *bro_snapshot_dto_new (void);
void bro_snapshot_dto_free (BroSnapshotDto *snapshot);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_snapshot_dto_json (const BroSnapshotDto *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSnapshotDto, bro_snapshot_dto_free)

G_END_DECLS
