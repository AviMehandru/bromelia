/* bro-job-kind.c */
#include "bro-job-kind.h"

#include <string.h>

static const char *const names[] = {
  "videoDisc",
  "audioCd",
  "dataDisc",
  "readErrorRetry",
  "verify",
  "replicate",
  "parity",
  "repair",
  "rescan",
  "runCommand",
  "transcode",
};

const char *
bro_job_kind_to_wire (BroJobKind value)
{
  g_return_val_if_fail ((guint) value < G_N_ELEMENTS (names), NULL);
  return names[value];
}

gboolean
bro_job_kind_from_wire (const char *text, BroJobKind *out)
{
  for (guint i = 0; text && i < G_N_ELEMENTS (names); i++)
    if (strcmp (names[i], text) == 0) {
      *out = (BroJobKind) i;
      return TRUE;
    }
  return FALSE;
}
