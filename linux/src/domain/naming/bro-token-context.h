/* bro-token-context.h: what bro_token_registry_values needs besides the identity: the rip word (Rip / Backup), the
 * drive's name, the disc and volume names, the disc type, the job's short id, the release year (from the online
 * lookup; -1: none) and the local time. */
#pragma once

#include "bro-disc-type.h"
#include "bro-local-time.h"

G_BEGIN_DECLS

typedef struct {
  const char *rip;
  const char *drive;
  const char *disc;
  const char *volume;
  BroDiscType type;
  const char *job;
  int release_year;
  gboolean has_local_time;
  BroLocalTime local_time;
} BroTokenContext;

G_END_DECLS
