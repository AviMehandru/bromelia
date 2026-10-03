/* bro-command-line.h: a program and its arguments. A bare program name is resolved by the adapter (ToolLocator). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *executable;
  GStrv arguments; /* never NULL */
} BroCommandLine;

/* Copies @executable; takes ownership of @arguments (NULL → none). */
BroCommandLine *bro_command_line_new (const char *executable, GStrv arguments);
void bro_command_line_free (BroCommandLine *line);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCommandLine, bro_command_line_free)

G_END_DECLS
