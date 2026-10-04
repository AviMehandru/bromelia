/* bro-process-spec.h: A process to start, never through a shell: the program and its arguments, environment variables
 * added to the engine's, the working directory, how it is stopped, how long it may stay silent before it is stopped as
 * stalled, the transcript file every line is appended to (headed by the command line), and the interpreter for a
 * script. */
#pragma once

#include "bro-duration.h"
#include "bro-stop-policy.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *executable;
  GStrv arguments;
  GHashTable *environment; /* char * → char * */
  char *working_directory; /* nullable */
  BroStopPolicy stop_policy;
  gboolean has_stall_timeout;
  BroDuration stall_timeout;
  char *transcript; /* nullable */
  char *interpreter; /* nullable */
} BroProcessSpec;

/* Empty lists, nothing else set. */
BroProcessSpec *bro_process_spec_new (void);
void bro_process_spec_free (BroProcessSpec *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroProcessSpec, bro_process_spec_free)

G_END_DECLS
