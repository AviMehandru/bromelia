/* bro-template-engine.c */
#include "bro-template-engine.h"
#include "bro-robot-private.h"
#include "bro-sanitizer.h"

#include <stdlib.h>
#include <string.h>

typedef char *(*Sanitize) (const char *);

static char *render (const char *t, gsize len, GHashTable *values, Sanitize sanitize);

static gssize
matching_brace (const char *s, gsize len, gsize open)
{
  int depth = 0;
  for (gsize i = open; i < len; i++) {
    if (s[i] == '{')
      depth++;
    if (s[i] == '}' && --depth == 0)
      return i;
  }
  return -1;
}

/* NULL when the token is unknown (the caller keeps it as it is). */
static char *
expand (const char *inner, GHashTable *values, Sanitize sanitize)
{
  const char *q = strchr (inner, '?');
  if (q) {
    /* A condition on a token that isn't set is false. */
    g_autofree char *key = g_strndup (inner, q - inner);
    const char *cond = g_hash_table_lookup (values, key);
    return cond && cond[0] ? render (q + 1, strlen (q + 1), values, sanitize) : g_strdup ("");
  }
  g_autofree char *key = NULL;
  int pad = 0;
  const char *colon = strchr (inner, ':');
  if (colon) {
    key = g_strndup (inner, colon - inner);
    if (!_bro_robot_parse_int (colon + 1, &pad))
      pad = 0;
  } else {
    key = g_strdup (inner);
  }
  const char *v = g_hash_table_lookup (values, key);
  if (!v)
    return NULL;
  g_autofree char *value = g_strdup (v);
  gboolean digits = v[0] != '\0';
  for (const char *p = v; *p; p++)
    digits = digits && g_ascii_isdigit (*p);
  if (pad > 0 && digits && strlen (v) < 10) {
    g_free (value);
    value = g_strdup_printf ("%0*d", pad, atoi (v));
  }
  return sanitize ? sanitize (value) : g_steal_pointer (&value);
}

static char *
render (const char *t, gsize len, GHashTable *values, Sanitize sanitize)
{
  GString *out = g_string_new (NULL);
  gsize i = 0;
  while (i < len) {
    if (t[i] == '{') {
      gssize close = matching_brace (t, len, i);
      if (close > (gssize) i) {
        g_autofree char *inner = g_strndup (t + i + 1, close - i - 1);
        g_autofree char *v = expand (inner, values, sanitize);
        if (v)
          g_string_append (out, v);
        else
          g_string_append_printf (out, "{%s}", inner);
        i = close + 1;
        continue;
      }
    }
    g_string_append_c (out, t[i++]);
  }
  return g_string_free (out, FALSE);
}

char *
bro_template_engine_render (const char *template_text, GHashTable *values)
{
  return render (template_text, strlen (template_text), values, NULL);
}

char *
bro_template_engine_render_path (const char *template_text, GHashTable *values)
{
  g_autofree char *slashed = g_strdup (template_text);
  for (char *p = slashed; *p; p++)
    if (*p == '\\')
      *p = '/';
  g_autofree char *rendered = render (slashed, strlen (slashed), values, bro_sanitizer_component);
  g_auto (GStrv) parts = g_strsplit (rendered, "/", -1);
  GString *out = g_string_new (NULL);
  for (int i = 0; parts[i]; i++) {
    g_autofree char *c = bro_sanitizer_component (parts[i]);
    if (!c[0] || strcmp (c, "..") == 0)
      continue;
    if (out->len)
      g_string_append_c (out, '/');
    g_string_append (out, c);
  }
  return g_string_free (out, FALSE);
}
