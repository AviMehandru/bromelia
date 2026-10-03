/* bro-test-english.c */
#include "bro-test-english.h"
#include "bro-test-fixtures.h"

#include <stdlib.h>
#include <string.h>

static BroJsonValue *texts;

static void format (GString *out, const char *p, gsize len, const BroJsonValue *args, const gint64 *hash);

static gsize
matching (const char *s, gsize len, gsize open)
{
  int depth = 0;
  for (gsize i = open; i < len; i++) {
    if (s[i] == '{')
      depth++;
    else if (s[i] == '}' && --depth == 0)
      return i;
  }
  g_error ("unbalanced braces in %.*s", (int) len, s);
}

static char *
trimmed (const char *s, gsize len)
{
  char *t = g_strndup (s, len);
  return g_strstrip (t);
}

static void
plain (GString *out, const BroJsonValue *v)
{
  if (!v)
    return;
  switch (v->kind) {
  case BRO_JSON_VALUE_STRING: g_string_append (out, v->string); break;
  case BRO_JSON_VALUE_INTEGER: g_string_append_printf (out, "%" G_GINT64_FORMAT, v->integer); break;
  case BRO_JSON_VALUE_BOOL: g_string_append (out, v->boolean ? "true" : "false"); break;
  case BRO_JSON_VALUE_ARRAY: {
    gboolean messages = TRUE;
    for (guint i = 0; i < v->items->len; i++)
      messages = messages && bro_json_value_member (v->items->pdata[i], "code");
    for (guint i = 0; i < v->items->len; i++) {
      if (i)
        g_string_append (out, messages ? "; " : ", ");
      if (messages) {
        g_autofree char *m = bro_test_english (v->items->pdata[i]);
        g_string_append (out, m);
      } else {
        plain (out, v->items->pdata[i]);
      }
    }
    break;
  }
  case BRO_JSON_VALUE_OBJECT:
    if (bro_json_value_member (v, "code")) {
      g_autofree char *m = bro_test_english (v);
      g_string_append (out, m);
    }
    break;
  default:
    break;
  }
}

/* The branch called @key of "key {text} key {text} …", or NULL. */
static gboolean
branch (const char *s, gsize len, const char *key, const char **text, gsize *text_len)
{
  gsize i = 0;
  while (i < len) {
    while (i < len && g_ascii_isspace (s[i]))
      i++;
    if (i >= len)
      break;
    const char *open = memchr (s + i, '{', len - i);
    if (!open)
      break;
    g_autofree char *k = trimmed (s + i, open - (s + i));
    gsize o = open - s;
    gsize close = matching (s, len, o);
    if (strcmp (k, key) == 0) {
      *text = s + o + 1;
      *text_len = close - o - 1;
      return TRUE;
    }
    i = close + 1;
  }
  return FALSE;
}

static void
bytes (GString *out, gint64 n)
{
  const char *units[] = { "B", "KB", "MB", "GB", "TB" };
  double v = (double) n;
  int u = 0;
  while (v >= 1000 && u < 4) {
    v /= 1000;
    u++;
  }
  if (u == 0)
    g_string_append_printf (out, "%" G_GINT64_FORMAT " B", n);
  else
    g_string_append_printf (out, "%.1f %s", v, units[u]);
}

static void
argument (GString *out, const char *body, gsize len, const BroJsonValue *args)
{
  /* name [, type [, style]] at the top level */
  gsize commas[2];
  int n = 0, depth = 0;
  for (gsize i = 0; i < len && n < 2; i++) {
    if (body[i] == '{')
      depth++;
    else if (body[i] == '}')
      depth--;
    else if (body[i] == ',' && depth == 0)
      commas[n++] = i;
  }
  g_autofree char *name = trimmed (body, n ? commas[0] : len);
  const BroJsonValue *value = bro_json_value_member (args, name);
  if (n == 0) {
    plain (out, value);
    return;
  }
  g_autofree char *type = trimmed (body + commas[0] + 1, (n > 1 ? commas[1] : len) - commas[0] - 1);
  const char *style = n > 1 ? body + commas[1] + 1 : "";
  gsize style_len = n > 1 ? len - commas[1] - 1 : 0;
  const char *text;
  gsize text_len;
  if (strcmp (type, "plural") == 0) {
    gint64 v = bro_json_value_get_integer (value, 0);
    g_autofree char *exact = g_strdup_printf ("=%" G_GINT64_FORMAT, v);
    if (branch (style, style_len, exact, &text, &text_len) || (v == 1 && branch (style, style_len, "one", &text, &text_len))
        || branch (style, style_len, "other", &text, &text_len))
      format (out, text, text_len, args, &v);
  } else if (strcmp (type, "select") == 0) {
    GString *key = g_string_new (NULL);
    plain (key, value);
    if (branch (style, style_len, key->str, &text, &text_len) || branch (style, style_len, "other", &text, &text_len))
      format (out, text, text_len, args, NULL);
    g_string_free (key, TRUE);
  } else if (strcmp (type, "bytes") == 0) {
    bytes (out, bro_json_value_get_integer (value, 0));
  } else if (strcmp (type, "duration") == 0 || strcmp (type, "durationPrecise") == 0) {
    gint64 s = (gint64) bro_json_value_get_number (value, 0);
    if (s >= 3600)
      g_string_append_printf (out, "%" G_GINT64_FORMAT ":%02d:%02d", s / 3600, (int) (s / 60 % 60), (int) (s % 60));
    else
      g_string_append_printf (out, "%" G_GINT64_FORMAT ":%02d", s / 60, (int) (s % 60));
  } else {
    plain (out, value);
  }
}

static void
format (GString *out, const char *p, gsize len, const BroJsonValue *args, const gint64 *hash)
{
  gsize i = 0;
  while (i < len) {
    if (p[i] == '#' && hash) {
      g_string_append_printf (out, "%" G_GINT64_FORMAT, *hash);
      i++;
    } else if (p[i] != '{') {
      g_string_append_c (out, p[i++]);
    } else {
      gsize end = matching (p, len, i);
      argument (out, p + i + 1, end - i - 1, args);
      i = end + 1;
    }
  }
}

char *
bro_test_english (const BroJsonValue *message)
{
  if (!texts)
    texts = bro_test_fixture_json ("../messages/en.json");
  const char *code = bro_json_value_get_string (bro_json_value_member (message, "code"), "");
  const char *text = bro_json_value_get_string (bro_json_value_member (texts, code), code);
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  const BroJsonValue *params = bro_json_value_member (message, "params");
  GString *out = g_string_new (NULL);
  format (out, text, strlen (text), params ? params : empty, NULL);
  const BroJsonValue *cause = bro_json_value_member (message, "cause");
  if (cause && cause->kind != BRO_JSON_VALUE_NULL) {
    g_autofree char *c = bro_test_english (cause);
    g_string_append_printf (out, " (%s)", c);
  }
  return g_string_free (out, FALSE);
}
