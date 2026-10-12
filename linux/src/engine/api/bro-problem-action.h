/* bro-problem-action.h: what a UI can offer for a problem (dtos.json Problem.actions). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_PROBLEM_ACTION_INSTALL_BETA_KEY,
  BRO_PROBLEM_ACTION_OPEN_SETTINGS,
  BRO_PROBLEM_ACTION_ATTACH_LIBRARY,
  BRO_PROBLEM_ACTION_REPAIR,
  BRO_PROBLEM_ACTION_RETRY,
} BroProblemAction;

/* The wire form ("installBetaKey"), as in shared/schema/api/dtos.json. */
const char *bro_problem_action_to_wire (BroProblemAction value);
gboolean bro_problem_action_from_wire (const char *text, BroProblemAction *out);

G_END_DECLS
