/* bro-decision.h: a decision about a disc and why (plan §3.1). Only the media kind is decided this way so far, so
 * the value is a BroMediaKind; the plan's confidence isn't used by anything yet and is left out. */
#pragma once

#include "bro-bro-message.h"
#include "bro-media-kind.h"

G_BEGIN_DECLS

typedef struct {
  BroMediaKind value;
  BroBroMessage *reason;
} BroDecision;

void bro_decision_free (BroDecision *decision);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDecision, bro_decision_free)

G_END_DECLS
