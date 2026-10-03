/* bro-notify-requests.h: BroNotifyRequests, how a notification URL is sent: Discord, Slack, ntfy and other webhooks
 * over HTTP; Telegram (one chat), Pushover and Gotify directly; any other Apprise URL with the apprise command. */
#pragma once

#include "bro-delivery.h"
#include "bro-notify-target.h"
#include "bro-status-word.h"

G_BEGIN_DECLS

/* How @url is reached for a notification with this title, body and status; NULL for an empty or malformed URL. */
BroDelivery *bro_notify_requests_build (const char *url, const char *title, const char *body, BroStatusWord status);

/* The enabled targets (BroNotifyTarget *, borrowed from @targets) that want a notification with this status: a
 * target with only_problems gets none on success. */
GPtrArray *bro_notify_requests_targets_for (GPtrArray *targets, BroStatusWord status);

G_END_DECLS
