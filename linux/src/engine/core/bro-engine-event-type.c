/* bro-engine-event-type.c */
#include "bro-engine-event-type.h"

#include <string.h>

static const char *const wire[] = {
  "snapshot", "driveChanged", "driveRemoved", "mediumArrived", "mediumRemoved", "sessionChanged", "sessionClosed",
  "jobChanged", "jobFinished", "jobLog", "queueChanged", "problemRaised", "problemCleared", "libraryChanged",
  "maintenanceProgress", "configChanged", "automationCountdown", "toolsChanged", "leaseChanged", "stepFinished",
};

G_STATIC_ASSERT (G_N_ELEMENTS (wire) == BRO_ENGINE_EVENT_TYPE_COUNT);

const char *
bro_engine_event_type_to_wire (BroEngineEventType value)
{
  return (guint) value < G_N_ELEMENTS (wire) ? wire[value] : NULL;
}

gboolean
bro_engine_event_type_from_wire (const char *text, BroEngineEventType *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (wire); i++)
    if (strcmp (text, wire[i]) == 0)
      {
        *out = i;
        return TRUE;
      }
  return FALSE;
}

gboolean
bro_engine_event_type_is_internal (BroEngineEventType type)
{
  return type == BRO_ENGINE_EVENT_TYPE_LEASE_CHANGED || type == BRO_ENGINE_EVENT_TYPE_STEP_FINISHED;
}
