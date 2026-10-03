/* bro-delivery.c */
#include "bro-delivery.h"

void
bro_delivery_free (BroDelivery *delivery)
{
  if (!delivery)
    return;
  g_clear_pointer (&delivery->request, bro_http_request_spec_free);
  g_free (delivery->url);
  g_free (delivery);
}
