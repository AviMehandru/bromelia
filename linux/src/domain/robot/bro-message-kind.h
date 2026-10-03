/* bro-message-kind.h: the severity of a MakeMKV message */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_MESSAGE_KIND_DEBUG,
  BRO_MESSAGE_KIND_INFO,
  BRO_MESSAGE_KIND_WARNING,
  BRO_MESSAGE_KIND_ERROR,
} BroMessageKind;

/* The wire form ("debug"), as in shared/schema/common.json. */
const char *bro_message_kind_to_wire (BroMessageKind value);
gboolean bro_message_kind_from_wire (const char *text, BroMessageKind *out);

G_END_DECLS
