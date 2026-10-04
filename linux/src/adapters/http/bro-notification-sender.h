/* bro-notification-sender.h: BroNotificationSender: notifications (plan §10.2;
 * shared/fixtures/adapters/notification-sender.cases.json): NotifyRequests.build, then the HttpClient or Apprise. A
 * failure is notify.badUrl, notify.httpFailed {host, status}, notify.failed {host, reason}, notify.needsApprise or
 * notify.appriseFailed; none names the URL, which is a secret. Synchronous. */
#pragma once

#include "bro-apprise-tool.h"
#include "bro-http-client.h"
#include "bro-status-word.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_NOTIFICATION_SENDER (bro_notification_sender_get_type ())
G_DECLARE_FINAL_TYPE (BroNotificationSender, bro_notification_sender, BRO, NOTIFICATION_SENDER, GObject)

/* Keeps references. */
BroNotificationSender *bro_notification_sender_new (BroHttpClient *http, BroAppriseTool *apprise);

gboolean bro_notification_sender_send (BroNotificationSender *self, const char *url, const char *title, const char *body, BroStatusWord status,
                                       BroCancellationToken *cancel, BroBroError **error);

G_END_DECLS
