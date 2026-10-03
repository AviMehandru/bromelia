/* bro-notify-target.h: a notification target of the configuration; its URL is the secret named secret (it carries
 * credentials), resolved by the keystore. The strings are borrowed. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  const char *id;
  const char *name;
  const char *secret;
  gboolean enabled;
  gboolean only_problems;
} BroNotifyTarget;

G_END_DECLS
