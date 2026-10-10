/* bro-engine-event.c */
#include "bro-engine-event.h"

BroEngineEvent *
bro_engine_event_new (gint64 seq, BroInstant at, BroEngineEventType type, BroJsonValue *data)
{
  BroEngineEvent *e = g_atomic_rc_box_new0 (BroEngineEvent);
  e->seq = seq;
  e->at = at;
  e->type = type;
  e->data = bro_json_value_ref (data);
  return e;
}

BroEngineEvent *
bro_engine_event_ref (BroEngineEvent *event)
{
  return g_atomic_rc_box_acquire (event);
}

static void
clear_event (BroEngineEvent *e)
{
  bro_json_value_unref (e->data);
}

void
bro_engine_event_unref (BroEngineEvent *event)
{
  if (event)
    g_atomic_rc_box_release_full (event, (GDestroyNotify) clear_event);
}

BroJsonValue *
bro_engine_event_json (const BroEngineEvent *event)
{
  BroJsonValue *o = bro_json_value_new_object ();
  g_autofree char *at = bro_instant_format (event->at);
  bro_json_value_set (o, "seq", bro_json_value_new_integer (event->seq));
  bro_json_value_set (o, "at", bro_json_value_new_string (at));
  bro_json_value_set (o, "type", bro_json_value_new_string (bro_engine_event_type_to_wire (event->type)));
  bro_json_value_set (o, "data", bro_json_value_ref (event->data));
  return o;
}
