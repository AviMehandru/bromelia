/* bro-command-settings.h: a command step: the program, its interpreter (empty = run it directly), the arguments
 * (split like a POSIX command line before tokens are filled in), the working directory (empty = the unit's
 * folder), whether it runs once per file, and its environment. Part of BroStepDefinition. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *executable;
  char *interpreter;
  char *arguments;
  char *working_directory;
  gboolean per_file;
  GHashTable *environment; /* char * → char * */
} BroCommandSettings;

G_END_DECLS
