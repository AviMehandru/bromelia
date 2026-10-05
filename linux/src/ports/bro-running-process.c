/* bro-running-process.c */
#include "bro-running-process.h"

G_DEFINE_INTERFACE (BroRunningProcess, bro_running_process, G_TYPE_OBJECT)

static void
bro_running_process_default_init (BroRunningProcessInterface *iface)
{
}

BroOutputLine *
bro_running_process_lines (BroRunningProcess *self)
{
  g_return_val_if_fail (BRO_IS_RUNNING_PROCESS (self), NULL);
  return BRO_RUNNING_PROCESS_GET_IFACE (self)->lines (self);
}

BroProcessExit
bro_running_process_wait (BroRunningProcess *self)
{
  g_return_val_if_fail (BRO_IS_RUNNING_PROCESS (self), (BroProcessExit) { 0 });
  return BRO_RUNNING_PROCESS_GET_IFACE (self)->wait (self);
}

void
bro_running_process_stop (BroRunningProcess *self, BroStopReason reason)
{
  g_return_if_fail (BRO_IS_RUNNING_PROCESS (self));
  BRO_RUNNING_PROCESS_GET_IFACE (self)->stop (self, reason);
}

BroBroMessage *
bro_running_process_transcript_problem (BroRunningProcess *self)
{
  g_return_val_if_fail (BRO_IS_RUNNING_PROCESS (self), NULL);
  if (!BRO_RUNNING_PROCESS_GET_IFACE (self)->transcript_problem)
    return NULL;
  return BRO_RUNNING_PROCESS_GET_IFACE (self)->transcript_problem (self);
}
