/* bro-job-summary.h: what a job notification says: the mode, what was ripped (the identified name, else the disc
 * label), how many files went where, and the job's error (NULL: none). Borrowed. */
#pragma once

#include "bro-bro-message.h"
#include "bro-rip-mode.h"

G_BEGIN_DECLS

typedef struct {
  BroRipMode mode;
  const char *what;
  int files;
  const char *path;
  const BroBroMessage *error;
} BroJobSummary;

G_END_DECLS
