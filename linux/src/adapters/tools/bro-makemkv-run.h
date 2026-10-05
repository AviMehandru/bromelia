/* bro-makemkv-run.h: one run: its outcome, the first key, version or drive notice, the LibreDrive detail, MakeMKV's
 * version, and process.noTranscript when its transcript couldn't be written. */
#pragma once

#include "bro-makemkv-notice.h"
#include "bro-run-outcome.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroRunOutcome *outcome;
  BroMakemkvNotice *notice; /* nullable */
  char *libre_drive;        /* nullable */
  char *version;            /* nullable */
  BroBroMessage *transcript_problem; /* nullable */
} BroMakemkvRun;

/* Everything zero. */
BroMakemkvRun *bro_makemkv_run_new (void);
void bro_makemkv_run_free (BroMakemkvRun *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMakemkvRun, bro_makemkv_run_free)

G_END_DECLS
