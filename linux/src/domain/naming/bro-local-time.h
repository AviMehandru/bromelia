/* bro-local-time.h: a wall-clock time in the user's time zone, for the date tokens (the adapters make it from the
 * clock). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int year, month, day, hour, minute, second;
} BroLocalTime;

G_END_DECLS
