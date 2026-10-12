/* bro-keystore.h: BroKeystore: Secrets (config SecretRef names), outside the configuration. */
#pragma once

#include "bro-bro-error.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_KEYSTORE (bro_keystore_get_type ())
G_DECLARE_INTERFACE (BroKeystore, bro_keystore, BRO, KEYSTORE, GObject)

struct _BroKeystoreInterface {
  GTypeInterface parent_iface;

  char *(*get) (BroKeystore *self, const char *name, BroBroError **error);
  gboolean (*set) (BroKeystore *self, const char *name, const char *value, BroBroError **error);
  gboolean (*remove) (BroKeystore *self, const char *name, BroBroError **error);
  gboolean (*is_system_store) (BroKeystore *self);
};

/* None when there is no such secret. Free with g_free. NULL when there is none. */
char *bro_keystore_get (BroKeystore *self, const char *name, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_keystore_set (BroKeystore *self, const char *name, const char *value, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_keystore_remove (BroKeystore *self, const char *name, BroBroError **error);

/* TRUE when the secrets are in the system's store (Keychain, Credential Manager, Secret Service), FALSE when they are
 * files in the secrets folder because there is none (Docker, a service without a logon). */
gboolean bro_keystore_is_system_store (BroKeystore *self);

G_END_DECLS
