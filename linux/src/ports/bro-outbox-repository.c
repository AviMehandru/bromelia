/* bro-outbox-repository.c */
#include "bro-outbox-repository.h"

G_DEFINE_INTERFACE (BroOutboxRepository, bro_outbox_repository, G_TYPE_OBJECT)

static void
bro_outbox_repository_default_init (BroOutboxRepositoryInterface *iface)
{
}

gboolean
bro_outbox_repository_add (BroOutboxRepository *self, const BroOutboxEntry *notification, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_OUTBOX_REPOSITORY (self), FALSE);
  return BRO_OUTBOX_REPOSITORY_GET_IFACE (self)->add (self, notification, error);
}

GPtrArray *
bro_outbox_repository_due (BroOutboxRepository *self, BroInstant now, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_OUTBOX_REPOSITORY (self), NULL);
  return BRO_OUTBOX_REPOSITORY_GET_IFACE (self)->due (self, now, error);
}

gboolean
bro_outbox_repository_mark_sent (BroOutboxRepository *self, BroId id, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_OUTBOX_REPOSITORY (self), FALSE);
  return BRO_OUTBOX_REPOSITORY_GET_IFACE (self)->mark_sent (self, id, error);
}

gboolean
bro_outbox_repository_mark_failed (BroOutboxRepository *self, BroId id, const BroBroError *last_error, const BroInstant *retry_at, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_OUTBOX_REPOSITORY (self), FALSE);
  return BRO_OUTBOX_REPOSITORY_GET_IFACE (self)->mark_failed (self, id, last_error, retry_at, error);
}
