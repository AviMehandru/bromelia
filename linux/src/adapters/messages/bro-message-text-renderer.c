/* bro-message-text-renderer.c */
#include "bro-message-text-renderer.h"

#include <math.h>
#include <string.h>

struct _BroMessageTextRenderer {
  GObject parent_instance;
  BroMessageTexts *texts;
};

static void message_renderer_iface_init (BroMessageRendererInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroMessageTextRenderer, bro_message_text_renderer, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MESSAGE_RENDERER, message_renderer_iface_init))

static void render_json (BroMessageTextRenderer *self, GString *out, const BroJsonValue *message);
static void format (BroMessageTextRenderer *self, GString *out, const char *p, gsize len, const BroJsonValue *args,
                    const gint64 *hash, const char *code);

/* An apostrophe that quotes: before another, a brace, or (in a plural's text) a #. */
static gboolean
quote_starts (const char *p, gsize len, gsize i, gboolean hash)
{
  if (p[i] != '\'' || i + 1 >= len)
    return FALSE;
  char n = p[i + 1];
  return n == '\'' || n == '{' || n == '}' || (hash && n == '#');
}

/* Past the quote at @i, appending what it stands for to @out (when not NULL): '' is one apostrophe; otherwise the text
 * up to the next lone apostrophe (or the end), in which '' is one too. */
static gsize
quote_end (const char *p, gsize len, gsize i, GString *out)
{
  if (p[i + 1] == '\'') {
    if (out)
      g_string_append_c (out, '\'');
    return i + 2;
  }
  gsize j = i + 1;
  while (j < len) {
    if (p[j] == '\'') {
      if (j + 1 < len && p[j + 1] == '\'') {
        if (out)
          g_string_append_c (out, '\'');
        j += 2;
        continue;
      }
      return j + 1;
    }
    if (out)
      g_string_append_c (out, p[j]);
    j++;
  }
  return j;
}

/* The brace that closes the one at @open; FALSE when none does. */
static gboolean
matching (const char *p, gsize len, gsize open, gsize *close)
{
  int depth = 0;
  gsize i = open;
  while (i < len) {
    if (quote_starts (p, len, i, FALSE)) {
      i = quote_end (p, len, i, NULL);
      continue;
    }
    if (p[i] == '{') {
      depth++;
    } else if (p[i] == '}' && --depth == 0) {
      *close = i;
      return TRUE;
    }
    i++;
  }
  return FALSE;
}

static char *
trimmed (const char *s, gsize len)
{
  return g_strstrip (g_strndup (s, len));
}

/* The branch called @key in "key {text} key {text} …": the first of a key counts, and an unclosed branch ends the
 * list. */
static gboolean
branch (const char *s, gsize len, const char *key, const char **text, gsize *text_len)
{
  gsize i = 0;
  while (i < len) {
    const char *open = memchr (s + i, '{', len - i);
    gsize close;
    if (!open || !matching (s, len, (gsize) (open - s), &close))
      return FALSE;
    g_autofree char *k = trimmed (s + i, (gsize) (open - (s + i)));
    if (strcmp (k, key) == 0) {
      *text = open + 1;
      *text_len = close - (gsize) (open - s) - 1;
      return TRUE;
    }
    i = close + 1;
  }
  return FALSE;
}

static gboolean
is_message (const BroJsonValue *v)
{
  return v->kind == BRO_JSON_VALUE_OBJECT && bro_json_value_get_string (bro_json_value_member (v, "code"), NULL) != NULL;
}

static void
plain (BroMessageTextRenderer *self, GString *out, const BroJsonValue *v, const char *message_join)
{
  switch (v->kind) {
  case BRO_JSON_VALUE_STRING:
    g_string_append (out, v->string);
    break;
  case BRO_JSON_VALUE_INTEGER:
    g_string_append_printf (out, "%" G_GINT64_FORMAT, v->integer);
    break;
  case BRO_JSON_VALUE_BOOL:
    g_string_append (out, v->boolean ? "true" : "false");
    break;
  case BRO_JSON_VALUE_NULL:
    break;
  case BRO_JSON_VALUE_ARRAY: {
    gboolean messages = v->items->len > 0;
    for (guint i = 0; i < v->items->len; i++)
      messages = messages && is_message (v->items->pdata[i]);
    for (guint i = 0; i < v->items->len; i++) {
      if (i)
        g_string_append (out, messages ? message_join : ", ");
      if (messages)
        render_json (self, out, v->items->pdata[i]);
      else
        plain (self, out, v->items->pdata[i], "; ");
    }
    break;
  }
  default:
    if (is_message (v)) {
      render_json (self, out, v);
    } else {
      g_autofree char *text = bro_json_value_to_text (v);
      g_string_append (out, g_strchomp (text));
    }
    break;
  }
}

/* Decimal units like a file manager: one decimal, rounded half up, the unit chosen after rounding (999950 is 1.0 MB). */
static void
bytes (GString *out, gint64 n)
{
  static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
  if (n < 1000) {
    g_string_append_printf (out, "%" G_GINT64_FORMAT " B", n);
    return;
  }
  int u = 1;
  gint64 tenths;
  for (;;) {
    gint64 q = 100;
    for (int k = 1; k < u; k++)
      q *= 1000;
    tenths = n / q + (n % q >= q / 2 ? 1 : 0);
    if (tenths < 10000 || u == (int) G_N_ELEMENTS (units) - 1)
      break;
    u++;
  }
  g_string_append_printf (out, "%" G_GINT64_FORMAT ".%d %s", tenths / 10, (int) (tenths % 10), units[u]);
}

/* A double that converts to an integer without undefined behaviour (nonsense such as NaN is 0). */
static gint64
whole (double d)
{
  return isfinite (d) && fabs (d) < 9e18 ? (gint64) d : 0;
}

/* m:ss, or h:mm:ss from an hour; fractions of a second dropped. */
static void
duration (GString *out, double seconds)
{
  gint64 s = whole (seconds);
  if (s >= 3600)
    g_string_append_printf (out, "%" G_GINT64_FORMAT ":%02d:%02d", s / 3600, (int) (s / 60 % 60), (int) (s % 60));
  else
    g_string_append_printf (out, "%" G_GINT64_FORMAT ":%02d", s / 60, (int) (s % 60));
}

/* h:mm:ss.sss, rounded to the millisecond. */
static void
duration_precise (GString *out, double seconds)
{
  gint64 ms = whole (round (seconds * 1000));
  g_string_append_printf (out, "%" G_GINT64_FORMAT ":%02d:%02d.%03d", ms / 3600000, (int) (ms / 60000 % 60),
                          (int) (ms / 1000 % 60), (int) (ms % 1000));
}

static void
argument (BroMessageTextRenderer *self, GString *out, const char *body, gsize len, const BroJsonValue *args, const char *code)
{
  /* name [, type [, style]], split at the commas outside braces */
  gsize commas[2] = { 0, 0 };
  int n = 0, depth = 0;
  gsize i = 0;
  while (i < len && n < 2) {
    if (body[i] == '{')
      depth++;
    else if (body[i] == '}')
      depth--;
    else if (body[i] == ',' && depth == 0)
      commas[n++] = i;
    i++;
  }
  g_autofree char *name = trimmed (body, n ? commas[0] : len);
  const BroJsonValue *value = args && args->kind == BRO_JSON_VALUE_OBJECT ? bro_json_value_member (args, name) : NULL;
  gboolean missing = !value || value->kind == BRO_JSON_VALUE_NULL;
  g_autofree char *type = n ? trimmed (body + commas[0] + 1, (n > 1 ? commas[1] : len) - commas[0] - 1) : g_strdup ("");
  const char *style = n > 1 ? body + commas[1] + 1 : "";
  gsize style_len = n > 1 ? len - commas[1] - 1 : 0;
  const char *text;
  gsize text_len;
  if (strcmp (type, "select") == 0) {
    g_autoptr (GString) key = g_string_new (NULL);
    if (!missing)
      plain (self, key, value, "; ");
    if ((!missing && branch (style, style_len, key->str, &text, &text_len)) || branch (style, style_len, "other", &text, &text_len))
      format (self, out, text, text_len, args, NULL, code);
    return;
  }
  if (missing) {
    g_string_append_printf (out, "{%s}", name);
  } else if (*type == '\0') {
    plain (self, out, value, bro_message_texts_is_message_list (self->texts, code, name) ? ", " : "; ");
  } else if (strcmp (type, "plural") == 0) {
    gint64 v = bro_json_value_get_integer (value, 0);
    g_autofree char *exact = g_strdup_printf ("=%" G_GINT64_FORMAT, v);
    if (branch (style, style_len, exact, &text, &text_len) || (v == 1 && branch (style, style_len, "one", &text, &text_len))
        || branch (style, style_len, "other", &text, &text_len))
      format (self, out, text, text_len, args, &v, code);
  } else if (strcmp (type, "bytes") == 0) {
    bytes (out, bro_json_value_get_integer (value, 0));
  } else if (strcmp (type, "duration") == 0) {
    duration (out, bro_json_value_get_number (value, 0));
  } else if (strcmp (type, "durationPrecise") == 0) {
    duration_precise (out, bro_json_value_get_number (value, 0));
  } else {
    plain (self, out, value, "; ");
  }
}

static void
format (BroMessageTextRenderer *self, GString *out, const char *p, gsize len, const BroJsonValue *args, const gint64 *hash,
        const char *code)
{
  gsize i = 0;
  while (i < len) {
    if (quote_starts (p, len, i, hash != NULL)) {
      i = quote_end (p, len, i, out);
    } else if (p[i] == '#' && hash) {
      g_string_append_printf (out, "%" G_GINT64_FORMAT, *hash);
      i++;
    } else if (p[i] == '{') {
      gsize end;
      if (!matching (p, len, i, &end)) {
        g_string_append_len (out, p + i, (gssize) (len - i));
        break;
      }
      argument (self, out, p + i + 1, end - i - 1, args, code);
      i = end + 1;
    } else {
      g_string_append_c (out, p[i++]);
    }
  }
}

static void
render_json (BroMessageTextRenderer *self, GString *out, const BroJsonValue *message)
{
  const char *code = bro_json_value_get_string (bro_json_value_member (message, "code"), "");
  const char *text = bro_message_texts_text (self->texts, code);
  if (text)
    format (self, out, text, strlen (text), bro_json_value_member (message, "params"), NULL, code);
  else
    g_string_append (out, code);
  const BroJsonValue *cause = bro_json_value_member (message, "cause");
  if (cause && cause->kind != BRO_JSON_VALUE_NULL) {
    g_string_append (out, " (");
    render_json (self, out, cause);
    g_string_append_c (out, ')');
  }
}

static void
bro_message_text_renderer_finalize (GObject *object)
{
  BroMessageTextRenderer *self = BRO_MESSAGE_TEXT_RENDERER (object);
  bro_message_texts_free (self->texts);
  G_OBJECT_CLASS (bro_message_text_renderer_parent_class)->finalize (object);
}

static void
bro_message_text_renderer_class_init (BroMessageTextRendererClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_message_text_renderer_finalize;
}

static void
bro_message_text_renderer_init (BroMessageTextRenderer *self)
{
}

BroMessageTextRenderer *
bro_message_text_renderer_new (BroMessageTexts *texts)
{
  BroMessageTextRenderer *self = g_object_new (BRO_TYPE_MESSAGE_TEXT_RENDERER, NULL);
  self->texts = texts;
  return self;
}

char *
bro_message_text_renderer_render_json (BroMessageTextRenderer *self, const BroJsonValue *message)
{
  GString *out = g_string_new (NULL);
  if (message && message->kind == BRO_JSON_VALUE_OBJECT)
    render_json (self, out, message);
  return g_string_free (out, FALSE);
}

char *
bro_message_text_renderer_render (BroMessageTextRenderer *self, const BroBroMessage *message)
{
  g_autoptr (BroJsonValue) json = bro_bro_message_to_json (message);
  return bro_message_text_renderer_render_json (self, json);
}

static char *
render_message (BroMessageRenderer *self, const BroBroMessage *message)
{
  return bro_message_text_renderer_render (BRO_MESSAGE_TEXT_RENDERER (self), message);
}

static void
message_renderer_iface_init (BroMessageRendererInterface *iface)
{
  iface->render = render_message;
}
