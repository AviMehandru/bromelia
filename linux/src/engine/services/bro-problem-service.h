/* bro-problem-service.h: BroProblemService: the active problems every UI shows until they are cleared (plan §11.1),
 * used on the executor only; a minimal one. In memory: each service raises its problems again when it starts. A problem
 * is keyed by its code and scope; its id is the code, or "code:kind:id" with a scope.
 * shared/fixtures/engine/problem-service.cases.json. */
#pragma once

#include "bro-clock.h"
#include "bro-event-bus.h"
#include "bro-problem-dto.h"

G_BEGIN_DECLS

#define BRO_TYPE_PROBLEM_SERVICE (bro_problem_service_get_type ())
G_DECLARE_FINAL_TYPE (BroProblemService, bro_problem_service, BRO, PROBLEM_SERVICE, GObject)

BroProblemService *bro_problem_service_new (BroEventBus *bus, BroClock *clock);

/* The active problems, in the order they were first raised: copies (BroProblemDto *), the caller frees the array. */
GPtrArray *bro_problem_service_problems (BroProblemService *self);

/* Adds the problem and publishes problemRaised. The same one again does nothing; one with other params, severity or
 * actions replaces it in place (same id, place and raisedAt) and publishes problemRaised again. Returns the problem
 * (a copy; free it). */
BroProblemDto *bro_problem_service_raise (BroProblemService *self, const BroBroMessage *message, const BroProblemScope *scope,
                                          const BroProblemAction *actions, gsize n_actions);

/* Removes the problem and publishes problemCleared; nothing when there is none. @scope may be NULL. */
void bro_problem_service_clear (BroProblemService *self, BroMessageCode code, const BroProblemScope *scope);

G_END_DECLS
