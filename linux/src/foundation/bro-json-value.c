/* bro-json-value.c: JSON values, a strict parser and the canonical writer. */
#include "bro-json-value.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BroJsonValue *
new_value (BroJsonValueKind kind)
{
  BroJsonValue *v = g_atomic_rc_box_new0 (BroJsonValue);
  v->kind = kind;
  return v;
}

BroJsonValue *bro_json_value_new_null (void) { return new_value (BRO_JSON_VALUE_NULL); }

BroJsonValue *
bro_json_value_new_bool (gboolean value)
{
  BroJsonValue *v = new_value (BRO_JSON_VALUE_BOOL);
  v->boolean = value ? TRUE : FALSE;
  return v;
}

BroJsonValue *
bro_json_value_new_integer (gint64 value)
{
  BroJsonValue *v = new_value (BRO_JSON_VALUE_INTEGER);
  v->integer = value;
  return v;
}

BroJsonValue *
bro_json_value_new_number (double value)
{
  BroJsonValue *v = new_value (BRO_JSON_VALUE_NUMBER);
  v->number = value;
  return v;
}

BroJsonValue *
bro_json_value_new_string (const char *value)
{
  BroJsonValue *v = new_value (BRO_JSON_VALUE_STRING);
  v->string = g_strdup (value ? value : "");
  return v;
}

BroJsonValue *
bro_json_value_new_array (void)
{
  BroJsonValue *v = new_value (BRO_JSON_VALUE_ARRAY);
  v->items = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_json_value_unref);
  return v;
}

BroJsonValue *
bro_json_value_new_object (void)
{
  BroJsonValue *v = new_value (BRO_JSON_VALUE_OBJECT);
  v->items = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_json_value_unref);
  v->keys = g_ptr_array_new_with_free_func (g_free);
  return v;
}

BroJsonValue *
bro_json_value_ref (BroJsonValue *value)
{
  return g_atomic_rc_box_acquire (value);
}

static void
clear_value (BroJsonValue *v)
{
  g_free (v->string);
  if (v->items)
    g_ptr_array_unref (v->items);
  if (v->keys)
    g_ptr_array_unref (v->keys);
}

void
bro_json_value_unref (BroJsonValue *value)
{
  if (value)
    g_atomic_rc_box_release_full (value, (GDestroyNotify) clear_value);
}

void
bro_json_value_append (BroJsonValue *array, BroJsonValue *item)
{
  g_return_if_fail (array->kind == BRO_JSON_VALUE_ARRAY);
  g_ptr_array_add (array->items, item);
}

void
bro_json_value_set (BroJsonValue *object, const char *key, BroJsonValue *value)
{
  g_return_if_fail (object->kind == BRO_JSON_VALUE_OBJECT);
  for (guint i = 0; i < object->keys->len; i++)
    if (strcmp (object->keys->pdata[i], key) == 0) {
      bro_json_value_unref (object->items->pdata[i]);
      object->items->pdata[i] = value;
      return;
    }
  g_ptr_array_add (object->keys, g_strdup (key));
  g_ptr_array_add (object->items, value);
}

BroJsonValue *
bro_json_value_member (const BroJsonValue *object, const char *key)
{
  if (!object || object->kind != BRO_JSON_VALUE_OBJECT)
    return NULL;
  for (guint i = 0; i < object->keys->len; i++)
    if (strcmp (object->keys->pdata[i], key) == 0)
      return object->items->pdata[i];
  return NULL;
}

const char *
bro_json_value_get_string (const BroJsonValue *value, const char *fallback)
{
  return value && value->kind == BRO_JSON_VALUE_STRING ? value->string : fallback;
}

gint64
bro_json_value_get_integer (const BroJsonValue *value, gint64 fallback)
{
  if (!value)
    return fallback;
  if (value->kind == BRO_JSON_VALUE_INTEGER)
    return value->integer;
  if (value->kind == BRO_JSON_VALUE_NUMBER && value->number == floor (value->number) && fabs (value->number) < 9.2e18)
    return (gint64) value->number;
  return fallback;
}

double
bro_json_value_get_number (const BroJsonValue *value, double fallback)
{
  if (!value)
    return fallback;
  if (value->kind == BRO_JSON_VALUE_INTEGER)
    return (double) value->integer;
  if (value->kind == BRO_JSON_VALUE_NUMBER)
    return value->number;
  return fallback;
}

gboolean
bro_json_value_get_bool (const BroJsonValue *value, gboolean fallback)
{
  return value && value->kind == BRO_JSON_VALUE_BOOL ? value->boolean : fallback;
}

guint
bro_json_value_length (const BroJsonValue *value)
{
  return value && value->items ? value->items->len : 0;
}

BroJsonValue *
bro_json_value_at (const BroJsonValue *value, guint index)
{
  return value && value->items && index < value->items->len ? value->items->pdata[index] : NULL;
}

gboolean
bro_json_value_equal (const BroJsonValue *a, const BroJsonValue *b)
{
  if (a == b)
    return TRUE;
  if (!a || !b || a->kind != b->kind)
    return FALSE;
  switch (a->kind) {
  case BRO_JSON_VALUE_NULL: return TRUE;
  case BRO_JSON_VALUE_BOOL: return a->boolean == b->boolean;
  case BRO_JSON_VALUE_INTEGER: return a->integer == b->integer;
  case BRO_JSON_VALUE_NUMBER: return a->number == b->number;
  case BRO_JSON_VALUE_STRING: return strcmp (a->string, b->string) == 0;
  case BRO_JSON_VALUE_ARRAY:
  case BRO_JSON_VALUE_OBJECT:
    if (a->items->len != b->items->len)
      return FALSE;
    for (guint i = 0; i < a->items->len; i++) {
      if (a->keys && strcmp (a->keys->pdata[i], b->keys->pdata[i]) != 0)
        return FALSE;
      if (!bro_json_value_equal (a->items->pdata[i], b->items->pdata[i]))
        return FALSE;
    }
    return TRUE;
  }
  return FALSE;
}

/* ---- parsing ------------------------------------------------------------------------------------------ */

typedef struct {
  const char *s;
  gsize n;
  gsize i;
} Parser;

static void
skip_space (Parser *p)
{
  while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r'))
    p->i++;
}

static int
peek (Parser *p)
{
  return p->i < p->n ? (unsigned char) p->s[p->i] : -1;
}

static gboolean
literal (Parser *p, const char *word)
{
  gsize len = strlen (word);
  if (p->i + len > p->n || memcmp (p->s + p->i, word, len) != 0)
    return FALSE;
  p->i += len;
  return TRUE;
}

static BroJsonValue *parse_value (Parser *p, int depth);

static gboolean
hex4 (Parser *p, gunichar *out)
{
  if (p->i + 4 > p->n)
    return FALSE;
  gunichar v = 0;
  for (int k = 0; k < 4; k++) {
    int d = g_ascii_xdigit_value (p->s[p->i + k]);
    if (d < 0)
      return FALSE;
    v = v * 16 + d;
  }
  p->i += 4;
  *out = v;
  return TRUE;
}

static char *
parse_string (Parser *p)
{
  p->i++;
  g_autoptr (GString) out = g_string_new (NULL);
  while (TRUE) {
    int c = peek (p);
    if (c < 0)
      return NULL;
    p->i++;
    if (c == '"')
      break;
    if (c < 0x20)
      return NULL;
    if (c != '\\') {
      g_string_append_c (out, (char) c);
      continue;
    }
    int e = peek (p);
    if (e < 0)
      return NULL;
    p->i++;
    switch (e) {
    case '"': g_string_append_c (out, '"'); break;
    case '\\': g_string_append_c (out, '\\'); break;
    case '/': g_string_append_c (out, '/'); break;
    case 'b': g_string_append_c (out, '\b'); break;
    case 'f': g_string_append_c (out, '\f'); break;
    case 'n': g_string_append_c (out, '\n'); break;
    case 'r': g_string_append_c (out, '\r'); break;
    case 't': g_string_append_c (out, '\t'); break;
    case 'u': {
      gunichar code;
      if (!hex4 (p, &code))
        return NULL;
      if (code >= 0xD800 && code < 0xDC00 && p->i + 1 < p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
        gsize save = p->i;
        gunichar low;
        p->i += 2;
        if (hex4 (p, &low) && low >= 0xDC00 && low < 0xE000)
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        else
          p->i = save;
      }
      if (code >= 0xD800 && code < 0xE000)
        code = 0xFFFD; /* a lone surrogate */
      g_string_append_unichar (out, code);
      break;
    }
    default:
      return NULL;
    }
  }
  return g_string_free (g_steal_pointer (&out), FALSE);
}

static BroJsonValue *
parse_number (Parser *p)
{
  gsize start = p->i;
  gboolean integral = TRUE;
  if (peek (p) == '-')
    p->i++;
  if (peek (p) == '0')
    p->i++;
  else if (peek (p) >= '1' && peek (p) <= '9')
    while (g_ascii_isdigit (peek (p)))
      p->i++;
  else
    return NULL;
  if (peek (p) == '.') {
    integral = FALSE;
    p->i++;
    if (!g_ascii_isdigit (peek (p)))
      return NULL;
    while (g_ascii_isdigit (peek (p)))
      p->i++;
  }
  if (peek (p) == 'e' || peek (p) == 'E') {
    integral = FALSE;
    p->i++;
    if (peek (p) == '+' || peek (p) == '-')
      p->i++;
    if (!g_ascii_isdigit (peek (p)))
      return NULL;
    while (g_ascii_isdigit (peek (p)))
      p->i++;
  }
  g_autofree char *text = g_strndup (p->s + start, p->i - start);
  if (integral) {
    char *end = NULL;
    errno = 0;
    gint64 n = g_ascii_strtoll (text, &end, 10);
    /* An integer that doesn't fit in 64 bits would lose digits as a double: not accepted. */
    return errno == 0 && end && *end == '\0' ? bro_json_value_new_integer (n) : NULL;
  }
  return bro_json_value_new_number (g_ascii_strtod (text, NULL));
}

static BroJsonValue *
parse_value (Parser *p, int depth)
{
  if (depth > 512)
    return NULL;
  int c = peek (p);
  if (c == '{') {
    p->i++;
    g_autoptr (BroJsonValue) obj = bro_json_value_new_object ();
    skip_space (p);
    if (peek (p) == '}') {
      p->i++;
      return g_steal_pointer (&obj);
    }
    while (TRUE) {
      skip_space (p);
      if (peek (p) != '"')
        return NULL;
      g_autofree char *key = parse_string (p);
      if (!key)
        return NULL;
      skip_space (p);
      if (!literal (p, ":"))
        return NULL;
      skip_space (p);
      BroJsonValue *v = parse_value (p, depth + 1);
      if (!v)
        return NULL;
      bro_json_value_set (obj, key, v);
      skip_space (p);
      int d = peek (p);
      p->i++;
      if (d == '}')
        return g_steal_pointer (&obj);
      if (d != ',')
        return NULL;
    }
  }
  if (c == '[') {
    p->i++;
    g_autoptr (BroJsonValue) arr = bro_json_value_new_array ();
    skip_space (p);
    if (peek (p) == ']') {
      p->i++;
      return g_steal_pointer (&arr);
    }
    while (TRUE) {
      skip_space (p);
      BroJsonValue *v = parse_value (p, depth + 1);
      if (!v)
        return NULL;
      bro_json_value_append (arr, v);
      skip_space (p);
      int d = peek (p);
      p->i++;
      if (d == ']')
        return g_steal_pointer (&arr);
      if (d != ',')
        return NULL;
    }
  }
  if (c == '"') {
    char *s = parse_string (p);
    if (!s)
      return NULL;
    BroJsonValue *v = new_value (BRO_JSON_VALUE_STRING);
    v->string = s;
    return v;
  }
  if (c == 't')
    return literal (p, "true") ? bro_json_value_new_bool (TRUE) : NULL;
  if (c == 'f')
    return literal (p, "false") ? bro_json_value_new_bool (FALSE) : NULL;
  if (c == 'n')
    return literal (p, "null") ? bro_json_value_new_null () : NULL;
  return parse_number (p);
}

BroJsonValue *
bro_json_value_parse (const char *text, gssize length)
{
  if (!text)
    return NULL;
  gsize n = length < 0 ? strlen (text) : (gsize) length;
  if (!g_utf8_validate_len (text, n, NULL))
    return NULL;
  Parser p = { text, n, 0 };
  if (n >= 3 && memcmp (text, "\xEF\xBB\xBF", 3) == 0)
    p.i = 3;
  skip_space (&p);
  g_autoptr (BroJsonValue) v = parse_value (&p, 0);
  if (!v)
    return NULL;
  skip_space (&p);
  return p.i == p.n ? g_steal_pointer (&v) : NULL;
}

/* ---- writing ------------------------------------------------------------------------------------------ */

static void
write_string (GString *out, const char *s)
{
  g_string_append_c (out, '"');
  for (const unsigned char *c = (const unsigned char *) s; *c; c++) {
    switch (*c) {
    case '"': g_string_append (out, "\\\""); break;
    case '\\': g_string_append (out, "\\\\"); break;
    case '\n': g_string_append (out, "\\n"); break;
    case '\r': g_string_append (out, "\\r"); break;
    case '\t': g_string_append (out, "\\t"); break;
    case '\b': g_string_append (out, "\\b"); break;
    case '\f': g_string_append (out, "\\f"); break;
    default:
      if (*c < 0x20)
        g_string_append_printf (out, "\\u%04x", *c);
      else
        g_string_append_c (out, (char) *c);
    }
  }
  g_string_append_c (out, '"');
}

/* Python's float repr: the shortest digits that read back as the same double, in fixed notation for exponents
 * from -4 to 15 and scientific notation otherwise. */
static void
write_number (GString *out, double d)
{
  if (isnan (d)) {
    g_string_append (out, "NaN");
    return;
  }
  if (isinf (d)) {
    g_string_append (out, d > 0 ? "Infinity" : "-Infinity");
    return;
  }
  if (d == 0) {
    g_string_append (out, signbit (d) ? "-0.0" : "0.0");
    return;
  }
  double a = fabs (d);
  char buf[64];
  char digits[32] = "";
  int exponent = 0;
  for (int precision = 1; precision <= 17; precision++) {
    char fmt[16];
    g_snprintf (fmt, sizeof fmt, "%%.%de", precision - 1);
    g_ascii_formatd (buf, sizeof buf, fmt, a);
    if (g_ascii_strtod (buf, NULL) != a)
      continue;
    char *e = strchr (buf, 'e');
    exponent = atoi (e + 1);
    int k = 0;
    for (char *c = buf; c < e; c++)
      if (g_ascii_isdigit (*c))
        digits[k++] = *c;
    digits[k] = '\0';
    while (k > 1 && digits[k - 1] == '0')
      digits[--k] = '\0';
    break;
  }
  if (d < 0)
    g_string_append_c (out, '-');
  int len = (int) strlen (digits);
  if (exponent < -4 || exponent >= 16) {
    g_string_append_c (out, digits[0]);
    if (len > 1) {
      g_string_append_c (out, '.');
      g_string_append (out, digits + 1);
    }
    g_string_append_printf (out, "e%c%02d", exponent < 0 ? '-' : '+', abs (exponent));
  } else if (exponent < 0) {
    g_string_append (out, "0.");
    for (int k = 0; k < -exponent - 1; k++)
      g_string_append_c (out, '0');
    g_string_append (out, digits);
  } else if (len <= exponent + 1) {
    g_string_append (out, digits);
    for (int k = 0; k < exponent + 1 - len; k++)
      g_string_append_c (out, '0');
    g_string_append (out, ".0");
  } else {
    g_string_append_len (out, digits, exponent + 1);
    g_string_append_c (out, '.');
    g_string_append (out, digits + exponent + 1);
  }
}

static void
write_value (GString *out, const BroJsonValue *v, int indent)
{
  switch (v->kind) {
  case BRO_JSON_VALUE_NULL: g_string_append (out, "null"); break;
  case BRO_JSON_VALUE_BOOL: g_string_append (out, v->boolean ? "true" : "false"); break;
  case BRO_JSON_VALUE_INTEGER: g_string_append_printf (out, "%" G_GINT64_FORMAT, v->integer); break;
  case BRO_JSON_VALUE_NUMBER: write_number (out, v->number); break;
  case BRO_JSON_VALUE_STRING: write_string (out, v->string); break;
  case BRO_JSON_VALUE_ARRAY:
  case BRO_JSON_VALUE_OBJECT: {
    gboolean obj = v->kind == BRO_JSON_VALUE_OBJECT;
    if (v->items->len == 0) {
      g_string_append (out, obj ? "{}" : "[]");
      break;
    }
    g_string_append_c (out, obj ? '{' : '[');
    for (guint k = 0; k < v->items->len; k++) {
      g_string_append (out, k == 0 ? "\n" : ",\n");
      for (int s = 0; s < indent + 2; s++)
        g_string_append_c (out, ' ');
      if (obj) {
        write_string (out, v->keys->pdata[k]);
        g_string_append (out, ": ");
      }
      write_value (out, v->items->pdata[k], indent + 2);
    }
    g_string_append_c (out, '\n');
    for (int s = 0; s < indent; s++)
      g_string_append_c (out, ' ');
    g_string_append_c (out, obj ? '}' : ']');
    break;
  }
  }
}

char *
bro_json_value_to_text (const BroJsonValue *value)
{
  GString *out = g_string_new (NULL);
  write_value (out, value, 0);
  g_string_append_c (out, '\n');
  return g_string_free (out, FALSE);
}

GBytes *
bro_json_value_encode_canonical (const BroJsonValue *value)
{
  char *text = bro_json_value_to_text (value);
  return g_bytes_new_take (text, strlen (text));
}
