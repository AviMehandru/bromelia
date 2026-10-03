/* bro-json-value.h: a JSON value for params and records (plan §3, L0). Objects keep their keys in the order
 * given, so a value written with bro_json_value_encode_canonical comes out the same on every platform. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_JSON_VALUE_NULL,
  BRO_JSON_VALUE_BOOL,
  BRO_JSON_VALUE_INTEGER, /* written without a fraction or exponent, fits in 64 bits */
  BRO_JSON_VALUE_NUMBER,  /* any other number */
  BRO_JSON_VALUE_STRING,
  BRO_JSON_VALUE_ARRAY,
  BRO_JSON_VALUE_OBJECT,
} BroJsonValueKind;

typedef struct _BroJsonValue BroJsonValue;

/* Reference counted and immutable once built. Read the fields directly. */
struct _BroJsonValue {
  BroJsonValueKind kind;
  gboolean boolean;
  gint64 integer;
  double number;
  char *string;     /* STRING: UTF-8 */
  GPtrArray *items; /* ARRAY: BroJsonValue *; OBJECT: the member values, in order */
  GPtrArray *keys;  /* OBJECT: char *, in order, parallel to items */
};

BroJsonValue *bro_json_value_new_null (void);
BroJsonValue *bro_json_value_new_bool (gboolean value);
BroJsonValue *bro_json_value_new_integer (gint64 value);
BroJsonValue *bro_json_value_new_number (double value);
BroJsonValue *bro_json_value_new_string (const char *value);
BroJsonValue *bro_json_value_new_array (void);
BroJsonValue *bro_json_value_new_object (void);
BroJsonValue *bro_json_value_ref (BroJsonValue *value);
void bro_json_value_unref (BroJsonValue *value);

/* Building (only before the value is shared). Both take ownership of @item / @value. */
void bro_json_value_append (BroJsonValue *array, BroJsonValue *item);
void bro_json_value_set (BroJsonValue *object, const char *key, BroJsonValue *value);

/* Access. member: the value called @key of an object, else NULL (borrowed). The getters return @fallback
 * when the value has another kind. */
BroJsonValue *bro_json_value_member (const BroJsonValue *object, const char *key);
const char *bro_json_value_get_string (const BroJsonValue *value, const char *fallback);
gint64 bro_json_value_get_integer (const BroJsonValue *value, gint64 fallback);
double bro_json_value_get_number (const BroJsonValue *value, double fallback);
gboolean bro_json_value_get_bool (const BroJsonValue *value, gboolean fallback);
guint bro_json_value_length (const BroJsonValue *value);
BroJsonValue *bro_json_value_at (const BroJsonValue *value, guint index);
gboolean bro_json_value_equal (const BroJsonValue *a, const BroJsonValue *b);

/* Strict JSON (RFC 8259) in UTF-8; @length -1 when NUL-terminated. NULL when the text isn't JSON. A repeated
 * key keeps its first position and its last value. */
BroJsonValue *bro_json_value_parse (const char *text, gssize length);

/* UTF-8 JSON with two-space indentation, keys in the order given and a final newline: the same bytes as
 * Python's json.dumps(value, indent=2, ensure_ascii=False) + "\n". */
GBytes *bro_json_value_encode_canonical (const BroJsonValue *value);

/* The same, as a NUL-terminated string. */
char *bro_json_value_to_text (const BroJsonValue *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJsonValue, bro_json_value_unref)

G_END_DECLS
