/* bro-step-definition.h: a post-processing step of the configuration (config-3.json's Step), with its defaults. */
#pragma once

#include "bro-command-settings.h"
#include "bro-hand-brake-settings.h"
#include "bro-json-value.h"
#include "bro-run-on.h"
#include "bro-step-type.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  BroStepType kind;
  char *name;
  gboolean enabled;
  BroRunOn run_on;
  gboolean background;
  gboolean affects_outcome;
  int timeout_seconds;
  BroCommandSettings command;
  BroHandBrakeSettings handbrake;
} BroStepDefinition;

/* A step from its JSON, with the schema's defaults for what it leaves out (background: true for handbrake, false
 * for command). */
BroStepDefinition *bro_step_definition_decode (BroJsonValue *json);
void bro_step_definition_free (BroStepDefinition *step);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroStepDefinition, bro_step_definition_free)

G_END_DECLS
