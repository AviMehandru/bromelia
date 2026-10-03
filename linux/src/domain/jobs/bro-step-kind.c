/* bro-step-kind.c */
#include "bro-step-kind.h"

#include <string.h>

static const char *const names[] = {
  "awaitMedia",
  "probe",
  "reconcile",
  "readNavigation",
  "identify",
  "plan",
  "decide",
  "acquire",
  "release",
  "verifyRips",
  "transform",
  "name",
  "seal",
  "commit",
  "publish",
  "protect",
  "postProcess",
  "notify",
  "ripAudio",
  "imageData",
  "mergeAttempts",
  "verifyUnit",
  "replicate",
  "parity",
  "repair",
  "rescan",
  "runCommand",
  "transcode",
};

const char *
bro_step_kind_to_wire (BroStepKind value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_step_kind_from_wire (const char *text, BroStepKind *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroStepKind) i;
      return TRUE;
    }
  return FALSE;
}
