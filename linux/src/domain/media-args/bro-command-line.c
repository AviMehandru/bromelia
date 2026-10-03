/* bro-command-line.c */
#include "bro-command-line.h"

BroCommandLine *
bro_command_line_new (const char *executable, GStrv arguments)
{
  BroCommandLine *l = g_new0 (BroCommandLine, 1);
  l->executable = g_strdup (executable);
  l->arguments = arguments ? arguments : g_new0 (char *, 1);
  return l;
}

void
bro_command_line_free (BroCommandLine *line)
{
  if (!line)
    return;
  g_free (line->executable);
  g_strfreev (line->arguments);
  g_free (line);
}
