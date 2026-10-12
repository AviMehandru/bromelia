/* bro-secret-status.c */
#include "bro-secret-status.h"

BroSecretStatus *
bro_secret_status_new (const char *name, gboolean set)
{
  BroSecretStatus *s = g_new0 (BroSecretStatus, 1);
  s->name = g_strdup (name);
  s->set = set;
  return s;
}

BroSecretStatus *
bro_secret_status_copy (const BroSecretStatus *status)
{
  return status ? bro_secret_status_new (status->name, status->set) : NULL;
}

void
bro_secret_status_free (BroSecretStatus *status)
{
  if (!status)
    return;
  g_free (status->name);
  g_free (status);
}
