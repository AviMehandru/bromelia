/* bro-timer-handle.c */
#include "bro-timer-handle.h"

G_DEFINE_INTERFACE (BroTimerHandle, bro_timer_handle, G_TYPE_OBJECT)

static void
bro_timer_handle_default_init (BroTimerHandleInterface *iface)
{
}

void
bro_timer_handle_cancel (BroTimerHandle *self)
{
  g_return_if_fail (BRO_IS_TIMER_HANDLE (self));
  BRO_TIMER_HANDLE_GET_IFACE (self)->cancel (self);
}
