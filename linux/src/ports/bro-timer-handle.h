/* bro-timer-handle.h: BroTimerHandle: A timer; cancel stops it. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_TIMER_HANDLE (bro_timer_handle_get_type ())
G_DECLARE_INTERFACE (BroTimerHandle, bro_timer_handle, BRO, TIMER_HANDLE, GObject)

struct _BroTimerHandleInterface {
  GTypeInterface parent_iface;

  void (*cancel) (BroTimerHandle *self);
};

void bro_timer_handle_cancel (BroTimerHandle *self);

G_END_DECLS
