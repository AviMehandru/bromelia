/* bro-notification-sender.c */
#include "bro-notification-sender.h"

#include "bro-message-code.h"
#include "bro-notify-requests.h"

struct _BroNotificationSender {
  GObject parent_instance;
  BroHttpClient *http;
  BroAppriseTool *apprise;
};

G_DEFINE_FINAL_TYPE (BroNotificationSender, bro_notification_sender, G_TYPE_OBJECT)

/* The host of @url, or "" (g_uri, as libsoup and the other platforms parse it). */
static char *
host_of (const char *url)
{
  g_autoptr (GUri) uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
  return g_strdup (uri && g_uri_get_host (uri) ? g_uri_get_host (uri) : "");
}

gboolean
bro_notification_sender_send (BroNotificationSender *self, const char *url, const char *title, const char *body, BroStatusWord status,
                              BroCancellationToken *cancel, BroBroError **error)
{
  g_autoptr (BroDelivery) delivery = bro_notify_requests_build (url, title, body, status);
  if (!delivery)
    {
      g_autofree char *scheme = _bro_apprise_tool_scheme (url);
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "target", bro_json_value_new_string (scheme));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_NOTIFY_BAD_URL), params);
      return FALSE;
    }
  if (delivery->kind == BRO_DELIVERY_APPRISE)
    return bro_apprise_tool_send (self->apprise, delivery->url, title, body, cancel, error);
  {
    g_autofree char *host = host_of (delivery->request->url);
    g_autoptr (BroBroError) failure = NULL;
    g_autoptr (BroHttpResponse) response = bro_http_client_send (self->http, delivery->request, cancel, &failure);
    if (!response)
      {
        if (failure && g_str_equal (failure->code, bro_message_code_wire (BRO_MSG_HTTP_FAILED)))
          {
            BroJsonValue *params = bro_json_value_new_object ();
            BroJsonValue *reason = bro_json_value_member (failure->params, "reason");
            bro_json_value_set (params, "host", bro_json_value_new_string (host));
            bro_json_value_set (params, "reason", reason ? bro_json_value_ref (reason) : bro_json_value_new_string (""));
            bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_NOTIFY_FAILED), params);
          }
        else if (error)
          *error = g_steal_pointer (&failure);
        return FALSE;
      }
    if (response->status < 200 || response->status >= 300)
      {
        BroJsonValue *params = bro_json_value_new_object ();
        bro_json_value_set (params, "host", bro_json_value_new_string (host));
        bro_json_value_set (params, "status", bro_json_value_new_integer (response->status));
        bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_NOTIFY_HTTP_FAILED), params);
        return FALSE;
      }
    return TRUE;
  }
}

static void
bro_notification_sender_finalize (GObject *object)
{
  BroNotificationSender *self = BRO_NOTIFICATION_SENDER (object);
  g_clear_object (&self->http);
  g_clear_object (&self->apprise);
  G_OBJECT_CLASS (bro_notification_sender_parent_class)->finalize (object);
}

static void
bro_notification_sender_class_init (BroNotificationSenderClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_notification_sender_finalize;
}

static void
bro_notification_sender_init (BroNotificationSender *self)
{
}

BroNotificationSender *
bro_notification_sender_new (BroHttpClient *http, BroAppriseTool *apprise)
{
  BroNotificationSender *self = g_object_new (BRO_TYPE_NOTIFICATION_SENDER, NULL);
  self->http = g_object_ref (http);
  self->apprise = g_object_ref (apprise);
  return self;
}
