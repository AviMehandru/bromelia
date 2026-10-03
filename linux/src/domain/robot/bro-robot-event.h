/* bro-robot-event.h: a parsed line of makemkvcon's robot output. The fields that apply depend on the kind. */
#pragma once

#include "bro-robot-message.h"

G_BEGIN_DECLS

typedef enum {
  BRO_ROBOT_EVENT_MESSAGE,          /* MSG: message */
  BRO_ROBOT_EVENT_PROGRESS_VALUE,   /* PRGV: current, total, max (the fractions are current/max, total/max) */
  BRO_ROBOT_EVENT_PROGRESS_CURRENT, /* PRGC: code, id, name (the current sub-operation) */
  BRO_ROBOT_EVENT_PROGRESS_TOTAL,   /* PRGT: code, id, name (the total operation) */
  BRO_ROBOT_EVENT_DRIVE,            /* DRV: index, state, flags, identification, label, device */
  BRO_ROBOT_EVENT_TITLE_COUNT,      /* TCOUNT: count */
  BRO_ROBOT_EVENT_DISC_INFO,        /* CINFO: id, code, value */
  BRO_ROBOT_EVENT_TITLE_INFO,       /* TINFO: title, id, code, value */
  BRO_ROBOT_EVENT_STREAM_INFO,      /* SINFO: title, stream, id, code, value */
  BRO_ROBOT_EVENT_RAW,              /* anything else: text */
} BroRobotEventKind;

typedef struct {
  BroRobotEventKind kind;
  BroRobotMessage *message;
  int current, total, max;
  int code, id;
  int index, state, flags;
  int count;
  int title, stream;
  char *name;
  char *identification, *label, *device;
  char *value;
  char *text;
} BroRobotEvent;

BroRobotEvent *bro_robot_event_new (BroRobotEventKind kind);
void bro_robot_event_free (BroRobotEvent *event);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRobotEvent, bro_robot_event_free)

G_END_DECLS
