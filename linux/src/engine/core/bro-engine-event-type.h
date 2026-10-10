/* bro-engine-event-type.h: every event type: the client-visible ones of shared/schema/events.json, then the
 * internal ones, which are never streamed. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_ENGINE_EVENT_TYPE_SNAPSHOT,
  BRO_ENGINE_EVENT_TYPE_DRIVE_CHANGED,
  BRO_ENGINE_EVENT_TYPE_DRIVE_REMOVED,
  BRO_ENGINE_EVENT_TYPE_MEDIUM_ARRIVED,
  BRO_ENGINE_EVENT_TYPE_MEDIUM_REMOVED,
  BRO_ENGINE_EVENT_TYPE_SESSION_CHANGED,
  BRO_ENGINE_EVENT_TYPE_SESSION_CLOSED,
  BRO_ENGINE_EVENT_TYPE_JOB_CHANGED,
  BRO_ENGINE_EVENT_TYPE_JOB_FINISHED,
  BRO_ENGINE_EVENT_TYPE_JOB_LOG,
  BRO_ENGINE_EVENT_TYPE_QUEUE_CHANGED,
  BRO_ENGINE_EVENT_TYPE_PROBLEM_RAISED,
  BRO_ENGINE_EVENT_TYPE_PROBLEM_CLEARED,
  BRO_ENGINE_EVENT_TYPE_LIBRARY_CHANGED,
  BRO_ENGINE_EVENT_TYPE_MAINTENANCE_PROGRESS,
  BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED,
  BRO_ENGINE_EVENT_TYPE_AUTOMATION_COUNTDOWN,
  BRO_ENGINE_EVENT_TYPE_TOOLS_CHANGED,
  BRO_ENGINE_EVENT_TYPE_LEASE_CHANGED,
  BRO_ENGINE_EVENT_TYPE_STEP_FINISHED,
} BroEngineEventType;

#define BRO_ENGINE_EVENT_TYPE_COUNT (BRO_ENGINE_EVENT_TYPE_STEP_FINISHED + 1)

/* The wire form ("jobChanged"). */
const char *bro_engine_event_type_to_wire (BroEngineEventType value);
gboolean bro_engine_event_type_from_wire (const char *text, BroEngineEventType *out);

/* leaseChanged and stepFinished: in-engine only, never streamed. */
gboolean bro_engine_event_type_is_internal (BroEngineEventType type);

G_END_DECLS
