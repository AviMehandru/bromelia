/* bro-delivery.h: how a notification goes out: an HTTP request, or an Apprise URL sent with the apprise command. */
#pragma once

#include "bro-http-request-spec.h"

G_BEGIN_DECLS

typedef enum {
  BRO_DELIVERY_HTTP,    /* request */
  BRO_DELIVERY_APPRISE, /* url */
} BroDeliveryKind;

typedef struct {
  BroDeliveryKind kind;
  BroHttpRequestSpec *request;
  char *url;
} BroDelivery;

void bro_delivery_free (BroDelivery *delivery);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDelivery, bro_delivery_free)

G_END_DECLS
