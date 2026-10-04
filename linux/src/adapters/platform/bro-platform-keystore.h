/* bro-platform-keystore.h: BroPlatformKeystore: the Keystore port on Linux (plan §10.3;
 * shared/fixtures/adapters/keystore.cases.json): secrets in the Secret Service through libsecret (schema
 * app.bromelia.Bromelia.Secret, attributes service and name, the default collection). When no Secret Service answers
 * (Docker, no session bus), the secrets are files <fallback_dir>/<name> (0600, in a 0700 folder) instead; that is
 * decided once, at the first call. A locked keyring fails (keystore.failed) rather than moving secrets elsewhere.
 * Names are SecretRef names; anything else fails with keystore.failed. Built without libsecret (the macOS build that
 * runs the tests), it always uses the files. */
#pragma once

#include "bro-keystore.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLATFORM_KEYSTORE (bro_platform_keystore_get_type ())
G_DECLARE_FINAL_TYPE (BroPlatformKeystore, bro_platform_keystore, BRO, PLATFORM_KEYSTORE, GObject)

/* @service: the service attribute (NULL: "Bromelia"; tests use their own). @system_store FALSE: always the files. */
BroPlatformKeystore *bro_platform_keystore_new (const char *fallback_dir, const char *service, gboolean system_store);

/* Where the secrets are: "secretService" or "file". */
const char *bro_platform_keystore_backend (BroPlatformKeystore *self);

/* NULL (and no error) when there is no such secret. Free with g_free. */
char *bro_platform_keystore_get (BroPlatformKeystore *self, const char *name, BroBroError **error);
gboolean bro_platform_keystore_set (BroPlatformKeystore *self, const char *name, const char *value, BroBroError **error);
gboolean bro_platform_keystore_remove (BroPlatformKeystore *self, const char *name, BroBroError **error);

G_END_DECLS
