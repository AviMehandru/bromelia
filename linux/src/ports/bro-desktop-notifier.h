/* bro-desktop-notifier.h: BroDesktopNotifier: Engine-side notifications, used when no app is connected. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_DESKTOP_NOTIFIER (bro_desktop_notifier_get_type ())
G_DECLARE_INTERFACE (BroDesktopNotifier, bro_desktop_notifier, BRO, DESKTOP_NOTIFIER, GObject)

struct _BroDesktopNotifierInterface {
  GTypeInterface parent_iface;

  void (*post) (BroDesktopNotifier *self, const char *title, const char *body, gboolean sound);
};

void bro_desktop_notifier_post (BroDesktopNotifier *self, const char *title, const char *body, gboolean sound);

G_END_DECLS
