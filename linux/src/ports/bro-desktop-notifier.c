/* bro-desktop-notifier.c */
#include "bro-desktop-notifier.h"

G_DEFINE_INTERFACE (BroDesktopNotifier, bro_desktop_notifier, G_TYPE_OBJECT)

static void
bro_desktop_notifier_default_init (BroDesktopNotifierInterface *iface)
{
}

void
bro_desktop_notifier_post (BroDesktopNotifier *self, const char *title, const char *body, gboolean sound)
{
  g_return_if_fail (BRO_IS_DESKTOP_NOTIFIER (self));
  BRO_DESKTOP_NOTIFIER_GET_IFACE (self)->post (self, title, body, sound);
}
