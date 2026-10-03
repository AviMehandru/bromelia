/* bro-command-args.c */
#include "bro-command-args.h"

#include "bro-argument-splitter.h"
#include "bro-media-args-private.h"
#include "bro-template-engine.h"

#include <string.h>

char *
_bro_media_args_expand_home (const char *path, const char *home)
{
  if (!home || path[0] != '~')
    return g_strdup (path);
  if (path[1] == '\0')
    return g_strdup (home);
  if (path[1] != '/' && path[1] != '\\')
    return g_strdup (path);
  gsize n = strlen (home);
  while (n > 0 && (home[n - 1] == '/' || home[n - 1] == '\\'))
    n--;
  return g_strdup_printf ("%.*s%s", (int) n, home, path + 1);
}

BroCommandLine *
bro_command_args_build (const BroStepDefinition *step, GHashTable *values, const char *const *files, const char *home)
{
  const BroCommandSettings *c = &step->command;
  g_autofree char *exe_text = bro_template_engine_render (c->executable ? c->executable : "", values);
  char *exe = _bro_media_args_expand_home (exe_text, home);
  g_autofree char *interpreter = _bro_media_args_expand_home (c->interpreter ? c->interpreter : "", home);
  GPtrArray *args = g_ptr_array_new ();
  if (*interpreter)
    g_ptr_array_add (args, g_strdup (exe));
  g_auto (GStrv) tokens = bro_argument_splitter_split (c->arguments ? c->arguments : "");
  for (int i = 0; tokens[i]; i++) {
    if (strcmp (tokens[i], "{files}") == 0) {
      for (int k = 0; files && files[k]; k++)
        g_ptr_array_add (args, g_strdup (files[k]));
    } else {
      g_ptr_array_add (args, bro_template_engine_render (tokens[i], values));
    }
  }
  g_ptr_array_add (args, NULL);
  BroCommandLine *line = bro_command_line_new (*interpreter ? interpreter : exe, (GStrv) g_ptr_array_free (args, FALSE));
  g_free (exe);
  return line;
}
