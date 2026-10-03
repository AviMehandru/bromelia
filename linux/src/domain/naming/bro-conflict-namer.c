/* bro-conflict-namer.c */
#include "bro-conflict-namer.h"

#include <string.h>

static gboolean
taken (const char *name, const char *const *existing)
{
  for (int i = 0; existing && existing[i]; i++)
    if (g_ascii_strcasecmp (existing[i], name) == 0)
      return TRUE;
  return FALSE;
}

char *
bro_conflict_namer_next (const char *name, const char *const *existing, gboolean is_folder)
{
  if (!taken (name, existing))
    return g_strdup (name);
  const char *dot = is_folder ? NULL : strrchr (name, '.');
  if (dot == name)
    dot = NULL;
  g_autofree char *stem = dot ? g_strndup (name, dot - name) : g_strdup (name);
  const char *ext = dot ? dot : "";
  for (int n = 2;; n++) {
    char *candidate = g_strdup_printf ("%s (%d)%s", stem, n, ext);
    if (!taken (candidate, existing))
      return candidate;
    g_free (candidate);
  }
}
