/* bro-replica-record.c */
#include "bro-replica-record.h"

BroReplicaRecord *
bro_replica_record_new (void)
{
  BroReplicaRecord *x = g_new0 (BroReplicaRecord, 1);
  return x;
}

void
bro_replica_record_free (BroReplicaRecord *x)
{
  if (!x)
    return;
  g_free (x->target_id);
  g_free (x->path);
  g_clear_pointer (&x->error, bro_bro_message_free);
  g_free (x);
}
