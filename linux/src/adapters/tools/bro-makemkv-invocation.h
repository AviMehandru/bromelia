/* bro-makemkv-invocation.h: what a run needs besides its source: the settings to isolate, the switches (the profile
 * path comes from the lease), how long it may stay silent, and the transcript file (the job's makemkv.txt). */
#pragma once

#include "bro-duration.h"
#include "bro-makemkv-options.h"
#include "bro-makemkv-run-settings.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroMakemkvRunSettings *settings; /* owned */
  BroMakemkvOptions options;       /* its strings are borrowed: they must outlive the call */
  gboolean has_stall_timeout;
  BroDuration stall_timeout;
  char *transcript; /* nullable */
} BroMakemkvInvocation;

/* Empty settings, default options (no --minlength), nothing else set. */
BroMakemkvInvocation *bro_makemkv_invocation_new (void);
void bro_makemkv_invocation_free (BroMakemkvInvocation *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMakemkvInvocation, bro_makemkv_invocation_free)

G_END_DECLS
