/* bro-command-args.h: BroCommandArgs, the command line of a command step. */
#pragma once

#include "bro-command-line.h"
#include "bro-step-definition.h"

G_BEGIN_DECLS

/* The arguments are split first and filled in from @values (char * → char *) afterwards, so a value with spaces
 * stays one argument, and a lone {files} becomes one argument per file of @files (NULL-terminated). With an
 * interpreter, the program is its first argument. A leading ~ is @home (nullable). (Choosing an interpreter by
 * extension when none is set is the process launcher's: it depends on the system and the file's mode.) */
BroCommandLine *bro_command_args_build (const BroStepDefinition *step, GHashTable *values, const char *const *files, const char *home);

G_END_DECLS
