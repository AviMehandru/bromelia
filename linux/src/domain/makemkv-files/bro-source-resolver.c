/* bro-source-resolver.c */
#include "bro-source-resolver.h"

#include <string.h>

static const char *
last_separator (const char *p)
{
  const char *a = strrchr (p, '/'), *b = strrchr (p, '\\');
  return a > b ? a : b;
}

static char *
parent (const char *p)
{
  const char *s = last_separator (p);
  if (!s)
    return g_strdup ("");
  if (s == p)
    return p[1] ? g_strndup (p, 1) : g_strdup ("");
  return g_strndup (p, s - p);
}

static const char *
name (const char *p)
{
  const char *s = last_separator (p);
  return s ? s + 1 : p;
}

BroMakemkvSource *
bro_source_resolver_source (const char *path, gboolean is_directory)
{
  g_autofree char *p = g_strdup (path);
  gsize n = strlen (p);
  while (n > 0 && (p[n - 1] == '/' || p[n - 1] == '\\'))
    p[--n] = '\0';
  g_autofree char *lower = g_ascii_strdown (name (p), -1);
  if (!is_directory && (g_str_has_suffix (lower, ".iso") || g_str_has_suffix (lower, ".img") || g_str_has_suffix (lower, ".udf")))
    return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_ISO, 0, NULL, p);
  g_autofree char *dir = is_directory ? g_strdup (p) : parent (p);
  for (int i = 0; i < 4 && *dir; i++) {
    const char *nm = name (dir);
    if (g_ascii_strcasecmp (nm, "BDMV") == 0 || g_ascii_strcasecmp (nm, "VIDEO_TS") == 0 || g_ascii_strcasecmp (nm, "HVDVD_TS") == 0) {
      g_autofree char *root = parent (dir);
      if (*root)
        return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_FILE, 0, NULL, root);
    }
    char *up = parent (dir);
    g_free (dir);
    dir = up;
    if (strcmp (dir, "/") == 0 || strcmp (dir, "\\") == 0)
      break;
  }
  return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_FILE, 0, NULL, p);
}
