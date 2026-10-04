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
};

/* None when there is no such secret. Free with g_free. NULL when there is none. */
char *bro_keystore_get (BroKeystore *self, const char *name, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_keystore_set (BroKeystore *self, const char *name, const char *value, BroBroError **error);

/* FALSE and @error set on failure. */
gboolean bro_keystore_remove (BroKeystore *self, const char *name, BroBroError **error);

G_END_DECLS
