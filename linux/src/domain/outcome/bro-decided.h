/* bro-decided.h: a job's outcome and the error that explains it (NULL for a plain success). */
#pragma once

#include "bro-bro-error.h"
#include "bro-outcome.h"

G_BEGIN_DECLS

typedef struct {
  BroOutcome outcome;
  BroBroError *error; /* nullable */
} BroDecided;

void bro_decided_free (BroDecided *decided);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDecided, bro_decided_free)

G_END_DECLS
