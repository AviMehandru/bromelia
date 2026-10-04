/* bro-check-record.h: A row of checks: one check of a unit, a replica or a folder. */
#pragma once

#include "bro-bro-message.h"
#include "bro-check-result.h"
#include "bro-id.h"
#include "bro-instant.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId id;
  gboolean has_unit_id;
  BroId unit_id;
  gboolean has_replica_id;
  BroId replica_id;
  char *folder;
  gboolean has_job_id;
  BroId job_id;
  BroInstant started_at;
  gboolean has_finished_at;
  BroInstant finished_at;
  BroCheckResult result;
  int files;
  gint64 bytes;
  GStrv changed;
  GStrv unreadable;
  GStrv missing;
  GStrv unlisted;
  BroBroMessage *error; /* nullable */
} BroCheckRecord;

/* Empty lists, nothing else set. */
BroCheckRecord *bro_check_record_new (void);
void bro_check_record_free (BroCheckRecord *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCheckRecord, bro_check_record_free)

G_END_DECLS
