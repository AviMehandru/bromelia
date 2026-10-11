/* bro-wait.h: why a request isn't granted yet: waiting for room, or blocked by a reason (library.offline). */
#pragma once

#include "bro-bro-error.h"

G_BEGIN_DECLS

typedef enum {
  BRO_WAIT_WAITING,
  BRO_WAIT_BLOCKED, /* reason */
} BroWaitKind;

typedef struct {
  BroWaitKind kind;
  BroBroError *reason; /* BRO_WAIT_BLOCKED only; owned */
} BroWait;

/* Copies @reason. */
BroWait *bro_wait_new (BroWaitKind kind, const BroBroError *reason);
void bro_wait_free (BroWait *wait);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroWait, bro_wait_free)

G_END_DECLS
