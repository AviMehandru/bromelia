/* bro-robot-message.h: a MSG line: code, flags, parameter count, MakeMKV's English text, its format and
 * parameters. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int code;
  int flags;
  int count;
  char *text;
  char *format;
  GStrv params; /* never NULL */
} BroRobotMessage;

BroRobotMessage *bro_robot_message_new (int code, int flags, const char *text);
BroRobotMessage *bro_robot_message_copy (const BroRobotMessage *message);
void bro_robot_message_free (BroRobotMessage *message);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRobotMessage, bro_robot_message_free)

G_END_DECLS
