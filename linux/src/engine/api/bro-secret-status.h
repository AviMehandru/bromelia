/* bro-secret-status.h: a secret the configuration refers to, and whether it has a value (never the value). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *name;
  gboolean set;
} BroSecretStatus;

BroSecretStatus *bro_secret_status_new (const char *name, gboolean set);
BroSecretStatus *bro_secret_status_copy (const BroSecretStatus *status);
void bro_secret_status_free (BroSecretStatus *status);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSecretStatus, bro_secret_status_free)

G_END_DECLS
