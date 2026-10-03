/* bro-robot-event.c */
#include "bro-robot-event.h"

BroRobotEvent *
bro_robot_event_new (BroRobotEventKind kind)
{
  BroRobotEvent *e = g_new0 (BroRobotEvent, 1);
  e->kind = kind;
  return e;
}

void
bro_robot_event_free (BroRobotEvent *event)
{
  if (!event)
    return;
  bro_robot_message_free (event->message);
  g_free (event->name);
  g_free (event->identification);
  g_free (event->label);
  g_free (event->device);
  g_free (event->value);
  g_free (event->text);
  g_free (event);
}
