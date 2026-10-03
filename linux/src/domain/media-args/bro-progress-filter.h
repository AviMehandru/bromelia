/* bro-progress-filter.h: bro_hand_brake_args_keep_line's state: the task being encoded (0: none yet) and the next
 * 10 % step to keep. Zero-initialised is the state before the first line. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int task;
  int next;
} BroProgressFilter;

G_END_DECLS
