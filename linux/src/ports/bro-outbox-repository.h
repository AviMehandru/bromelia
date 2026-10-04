/* bro-outbox-repository.h: BroOutboxRepository: The outbox table. */
#pragma once

#include "bro-bro-error.h"
#include "bro-id.h"
#include "bro-instant.h"
#include "bro-outbox-entry.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_OUTBOX_REPOSITORY (bro_outbox_repository_get_type ())
G_DECLARE_INTERFACE (BroOutboxRepository, bro_outbox_repository, BRO, OUTBOX_REPOSITORY, GObject)

struct _BroOutboxRepositoryInterface {
  GTypeInterface parent_iface;

  gboolean (*add) (BroOutboxRepository *self, const BroOutboxEntry *notification, BroBroError **error);
  GPtrArray *(*due) (BroOutboxRepository *self, BroInstant now, BroBroError **error);
  gboolean (*mark_sent) (BroOutboxRepository *self, BroId id, BroBroError **error);
  gboolean (*mark_failed) (BroOutboxRepository *self, BroId id, const BroBroError *last_error, const BroInstant *retry_at, BroBroError **error);
};

/* FALSE and @error set on failure. */
gboolean bro_outbox_repository_add (BroOutboxRepository *self, const BroOutboxEntry *notification, BroBroError **error);

/* Unsent notifications whose next attempt is due. */
GPtrArray *bro_outbox_repository_due (BroOutboxRepository *self, BroInstant now, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_outbox_repository_mark_sent (BroOutboxRepository *self, BroId id, BroBroError **error);

/* No retryAt: give up. FALSE and @error set on failure. */
gboolean bro_outbox_repository_mark_failed (BroOutboxRepository *self, BroId id, const BroBroError *last_error, const BroInstant *retry_at, BroBroError **error);

G_END_DECLS
