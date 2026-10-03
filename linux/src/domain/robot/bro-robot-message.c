/* bro-robot-message.c */
#include "bro-robot-message.h"

BroRobotMessage *
bro_robot_message_new (int code, int flags, const char *text)
{
  BroRobotMessage *m = g_new0 (BroRobotMessage, 1);
  m->code = code;
  m->flags = flags;
  m->text = g_strdup (text ? text : "");
  m->format = g_strdup ("");
  m->params = g_new0 (char *, 1);
  return m;
}

BroRobotMessage *
bro_robot_message_copy (const BroRobotMessage *message)
{
  if (!message)
    return NULL;
  BroRobotMessage *m = g_new0 (BroRobotMessage, 1);
  m->code = message->code;
  m->flags = message->flags;
  m->count = message->count;
  m->text = g_strdup (message->text);
  m->format = g_strdup (message->format);
  m->params = g_strdupv (message->params);
  return m;
}

void
bro_robot_message_free (BroRobotMessage *message)
{
  if (!message)
    return;
  g_free (message->text);
  g_free (message->format);
  g_strfreev (message->params);
  g_free (message);
}
