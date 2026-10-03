/* bro-schema-walker.c: walks the compiled configuration schema (bro-config-private.h). */
#include "bro-config-private.h"
#include "bro-issue.h"

#include <math.h>
#include <string.h>

#define M bro_json_value_member

BroJsonValue *
_bro_schema_document (void)
{
  static gsize once = 0;
  static BroJsonValue *doc = NULL;
  if (g_once_init_enter (&once)) {
    doc = bro_json_value_parse (_bro_config_schema_text (), -1);
    g_assert (doc != NULL);
    g_once_init_leave (&once, 1);
  }
  return doc;
}

BroJsonValue *
_bro_schema_def (const char *name)
{
  return M (M (_bro_schema_document (), "$defs"), name);
}

/* Follows $ref. */
static BroJsonValue *
resolve (BroJsonValue *node)
{
  for (int i = 0; i < 20 && node; i++) {
    const char *r = bro_json_value_get_string (M (node, "$ref"), NULL);
    if (!r)
      break;
    node = _bro_schema_def (r + strlen ("#/$defs/"));
  }
  return node;
}

typedef BroJsonValue *(*Finder) (BroJsonValue *node);

static BroJsonValue *
search (BroJsonValue *n, const char *const *keys, Finder find)
{
  for (int k = 0; keys[k]; k++) {
    BroJsonValue *list = M (n, keys[k]);
    for (guint i = 0; i < bro_json_value_length (list); i++) {
      BroJsonValue *found = find (bro_json_value_at (list, i));
      if (found)
        return found;
    }
  }
  return NULL;
}

static const char *const all_one_any[] = { "allOf", "oneOf", "anyOf", NULL };
static const char *const one_any_all[] = { "oneOf", "anyOf", "allOf", NULL };

/* The object-typed branch of a node (through $ref, allOf, oneOf, anyOf), or NULL. */
static BroJsonValue *
object_variant (BroJsonValue *node)
{
  BroJsonValue *n = resolve (node);
  if (M (n, "properties"))
    return n;
  return search (n, all_one_any, object_variant);
}

static BroJsonValue *
array_items (BroJsonValue *node)
{
  BroJsonValue *n = resolve (node);
  if (M (n, "items"))
    return M (n, "items");
  return search (n, one_any_all, array_items);
}

/* The additionalProperties schema of a map (MakemkvSettings, environment), or NULL. */
static BroJsonValue *
map_values (BroJsonValue *node)
{
  BroJsonValue *n = resolve (node);
  BroJsonValue *ap = M (n, "additionalProperties");
  if (ap && ap->kind == BRO_JSON_VALUE_OBJECT)
    return ap;
  return search (n, all_one_any, map_values);
}

static gboolean
inherits (BroJsonValue *node)
{
  BroJsonValue *n = resolve (node);
  if (bro_json_value_get_bool (M (n, "x-inherit"), FALSE))
    return TRUE;
  BroJsonValue *all = M (n, "allOf");
  for (guint i = 0; i < bro_json_value_length (all); i++)
    if (inherits (bro_json_value_at (all, i)))
      return TRUE;
  return FALSE;
}

static gboolean
is_object (BroJsonValue *node)
{
  BroJsonValue *n = resolve (node);
  return g_strcmp0 (bro_json_value_get_string (M (n, "type"), NULL), "object") == 0 || M (n, "properties") != NULL;
}

static gboolean
nullable (BroJsonValue *node)
{
  BroJsonValue *n = resolve (node);
  BroJsonValue *t = M (n, "type");
  if (g_strcmp0 (bro_json_value_get_string (t, NULL), "null") == 0)
    return TRUE;
  for (guint i = 0; t && t->kind == BRO_JSON_VALUE_ARRAY && i < t->items->len; i++)
    if (g_strcmp0 (bro_json_value_get_string (t->items->pdata[i], NULL), "null") == 0)
      return TRUE;
  const char *keys[] = { "oneOf", "anyOf" };
  for (int k = 0; k < 2; k++) {
    BroJsonValue *list = M (n, keys[k]);
    for (guint i = 0; i < bro_json_value_length (list); i++)
      if (nullable (bro_json_value_at (list, i)))
        return TRUE;
  }
  return FALSE;
}

static BroJsonValue *
default_of (BroJsonValue *prop)
{
  BroJsonValue *d = M (prop, "default");
  return d ? d : M (resolve (prop), "default");
}

static char *
join (const char *path, const char *key)
{
  return path[0] ? g_strconcat (path, ".", key, NULL) : g_strdup (key);
}

/* Whether the schema forbids @key in this object (allOf: if … then not required), so it mustn't be filled: a
 * command step gets no handbrake object. */
static gboolean
forbidden (BroJsonValue *obj, const char *key, BroJsonValue *value)
{
  BroJsonValue *all = M (obj, "allOf");
  for (guint i = 0; i < bro_json_value_length (all); i++) {
    BroJsonValue *sub = bro_json_value_at (all, i);
    BroJsonValue *cond = M (sub, "if");
    BroJsonValue *required = M (M (M (sub, "then"), "not"), "required");
    if (!cond || !required)
      continue;
    gboolean listed = FALSE;
    for (guint k = 0; k < bro_json_value_length (required); k++)
      listed = listed || g_strcmp0 (bro_json_value_get_string (bro_json_value_at (required, k), NULL), key) == 0;
    if (!listed)
      continue;
    g_autoptr (GPtrArray) l = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
    _bro_schema_validate (value, cond, "", l);
    if (l->len == 0)
      return TRUE;
  }
  return FALSE;
}

BroJsonValue *
_bro_schema_normalize_full (BroJsonValue *value, BroJsonValue *node, const char *path, gboolean fill, gboolean full,
                            GPtrArray *issues)
{
  if (value->kind == BRO_JSON_VALUE_OBJECT) {
    BroJsonValue *obj = object_variant (node);
    if (obj) {
      gboolean f = fill && (full || (!inherits (node) && !inherits (obj)));
      BroJsonValue *props = M (obj, "properties");
      BroJsonValue *out = bro_json_value_new_object ();
      g_autoptr (GHashTable) child_issues = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_ptr_array_unref);
      for (guint i = 0; i < props->keys->len; i++) {
        const char *key = props->keys->pdata[i];
        BroJsonValue *prop = props->items->pdata[i];
        g_autoptr (BroJsonValue) made = NULL;
        BroJsonValue *child = M (value, key);
        if (!child) {
          if (!f || forbidden (obj, key, value))
            continue;
          BroJsonValue *d = default_of (prop);
          if (d)
            child = d;
          else if (strcmp (key, "background") == 0 && bro_json_value_get_bool (M (obj, "x-default-background"), FALSE))
            /* A step's background defaults by its kind: true for handbrake, false for command. */
            child = made = bro_json_value_new_bool (g_strcmp0 (bro_json_value_get_string (M (value, "kind"), NULL), "handbrake") == 0);
          else if (is_object (prop) && !nullable (prop))
            child = made = bro_json_value_new_object ();
          else
            continue;
        }
        GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
        g_autofree char *p = join (path, key);
        bro_json_value_set (out, key, _bro_schema_normalize_full (child, prop, p, f, full, list));
        g_hash_table_insert (child_issues, (gpointer) key, list);
      }
      /* Issues in document order: an unknown key where it was written, a known key's own issues there too. */
      for (guint i = 0; i < value->keys->len; i++) {
        const char *key = value->keys->pdata[i];
        GPtrArray *list = g_hash_table_lookup (child_issues, key);
        if (list) {
          for (guint k = 0; k < list->len; k++)
            g_ptr_array_add (issues, bro_issue_copy (list->pdata[k]));
        } else if (!M (props, key)) {
          g_autofree char *p = join (path, key);
          g_ptr_array_add (issues, bro_issue_new (p, BRO_MSG_CONFIG_UNKNOWN_KEY, NULL, BRO_SEVERITY_WARNING));
        }
      }
      return out;
    }
    BroJsonValue *values = map_values (node);
    if (values) {
      BroJsonValue *out = bro_json_value_new_object ();
      for (guint i = 0; i < value->keys->len; i++) {
        g_autofree char *p = join (path, value->keys->pdata[i]);
        bro_json_value_set (out, value->keys->pdata[i], _bro_schema_normalize_full (value->items->pdata[i], values, p, fill, full, issues));
      }
      return out;
    }
    return bro_json_value_ref (value);
  }
  if (value->kind == BRO_JSON_VALUE_ARRAY) {
    BroJsonValue *items = array_items (node);
    if (!items)
      return bro_json_value_ref (value);
    BroJsonValue *out = bro_json_value_new_array ();
    for (guint i = 0; i < value->items->len; i++) {
      g_autofree char *p = g_strdup_printf ("%s[%u]", path, i);
      bro_json_value_append (out, _bro_schema_normalize_full (value->items->pdata[i], items, p, fill, full, issues));
    }
    return out;
  }
  return bro_json_value_ref (value);
}

BroJsonValue *
_bro_schema_normalize (BroJsonValue *value, BroJsonValue *node, const char *path, gboolean fill, GPtrArray *issues)
{
  return _bro_schema_normalize_full (value, node, path, fill, FALSE, issues);
}

/* ---- validation --------------------------------------------------------------------------------------- */

static gboolean
type_is (const char *type, const BroJsonValue *v)
{
  switch (v->kind) {
  case BRO_JSON_VALUE_OBJECT: return strcmp (type, "object") == 0;
  case BRO_JSON_VALUE_ARRAY: return strcmp (type, "array") == 0;
  case BRO_JSON_VALUE_STRING: return strcmp (type, "string") == 0;
  case BRO_JSON_VALUE_BOOL: return strcmp (type, "boolean") == 0;
  case BRO_JSON_VALUE_INTEGER: return strcmp (type, "integer") == 0 || strcmp (type, "number") == 0;
  case BRO_JSON_VALUE_NUMBER: return strcmp (type, "number") == 0;
  case BRO_JSON_VALUE_NULL: return strcmp (type, "null") == 0;
  }
  return FALSE;
}

static gboolean
type_matches (BroJsonValue *value, BroJsonValue *node)
{
  BroJsonValue *t = M (node, "type");
  if (!t)
    return TRUE;
  if (t->kind == BRO_JSON_VALUE_STRING)
    return type_is (t->string, value);
  for (guint i = 0; i < bro_json_value_length (t); i++)
    if (type_is (bro_json_value_get_string (bro_json_value_at (t, i), ""), value))
      return TRUE;
  return FALSE;
}

static BroIssue *
invalid (const char *path, BroJsonValue *value)
{
  BroJsonValue *p = bro_json_value_new_object ();
  if (value->kind == BRO_JSON_VALUE_STRING) {
    bro_json_value_set (p, "value", bro_json_value_new_string (value->string));
  } else {
    g_autofree char *text = bro_json_value_to_text (value);
    g_strchomp (text);
    bro_json_value_set (p, "value", bro_json_value_new_string (text));
  }
  return bro_issue_new (path, BRO_MSG_CONFIG_INVALID_VALUE, p, BRO_SEVERITY_ERROR);
}

static void
take_all (GPtrArray *to, GPtrArray *from)
{
  for (guint i = 0; i < from->len; i++)
    g_ptr_array_add (to, bro_issue_copy (from->pdata[i]));
}

void
_bro_schema_validate (BroJsonValue *value, BroJsonValue *node, const char *path, GPtrArray *issues)
{
  BroJsonValue *n = resolve (node);
  /* oneOf / anyOf */
  g_autoptr (GPtrArray) alternatives = g_ptr_array_new ();
  const char *keys[] = { "oneOf", "anyOf" };
  for (int k = 0; k < 2; k++)
    for (guint i = 0; i < bro_json_value_length (M (n, keys[k])); i++)
      g_ptr_array_add (alternatives, bro_json_value_at (M (n, keys[k]), i));
  if (alternatives->len) {
    g_autoptr (GPtrArray) results = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
    gboolean ok = FALSE, secret = FALSE;
    for (guint i = 0; i < alternatives->len; i++) {
      GPtrArray *l = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
      _bro_schema_validate (value, alternatives->pdata[i], path, l);
      ok = ok || l->len == 0;
      secret = secret || g_strcmp0 (bro_json_value_get_string (M (alternatives->pdata[i], "$ref"), NULL), "#/$defs/SecretRef") == 0;
      g_ptr_array_add (results, l);
    }
    if (!ok) {
      if (value->kind == BRO_JSON_VALUE_STRING && secret) {
        g_ptr_array_add (issues, bro_issue_new (path, BRO_MSG_CONFIG_SECRET_INLINE, NULL, BRO_SEVERITY_ERROR));
      } else {
        guint typed = alternatives->len;
        for (guint i = 0; i < alternatives->len && typed == alternatives->len; i++)
          if (type_matches (value, resolve (alternatives->pdata[i])))
            typed = i;
        if (typed < alternatives->len)
          take_all (issues, results->pdata[typed]);
        else
          g_ptr_array_add (issues, invalid (path, value));
      }
    }
  }
  BroJsonValue *all = M (n, "allOf");
  for (guint i = 0; i < bro_json_value_length (all); i++) {
    BroJsonValue *sub = bro_json_value_at (all, i);
    BroJsonValue *cond = M (sub, "if");
    if (cond) {
      g_autoptr (GPtrArray) l = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
      _bro_schema_validate (value, cond, path, l);
      if (l->len == 0 && M (sub, "then"))
        _bro_schema_validate (value, M (sub, "then"), path, issues);
      continue;
    }
    _bro_schema_validate (value, sub, path, issues);
  }
  if (M (n, "not")) {
    g_autoptr (GPtrArray) l = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
    _bro_schema_validate (value, M (n, "not"), path, l);
    if (l->len == 0)
      g_ptr_array_add (issues, invalid (path, value));
  }
  if (!type_matches (value, n)) {
    g_ptr_array_add (issues, invalid (path, value));
    return;
  }
  if (M (n, "const") && !bro_json_value_equal (M (n, "const"), value)) {
    g_ptr_array_add (issues, invalid (path, value));
    return;
  }
  BroJsonValue *e = M (n, "enum");
  if (e) {
    gboolean found = FALSE;
    for (guint i = 0; i < bro_json_value_length (e) && !found; i++)
      found = bro_json_value_equal (bro_json_value_at (e, i), value);
    if (!found) {
      g_ptr_array_add (issues, invalid (path, value));
      return;
    }
  }
  switch (value->kind) {
  case BRO_JSON_VALUE_OBJECT: {
    BroJsonValue *req = M (n, "required");
    for (guint i = 0; i < bro_json_value_length (req); i++) {
      const char *key = bro_json_value_get_string (bro_json_value_at (req, i), "");
      if (!M (value, key)) {
        g_autofree char *p = join (path, key);
        g_ptr_array_add (issues, bro_issue_new (p, BRO_MSG_CONFIG_REQUIRED, NULL, BRO_SEVERITY_ERROR));
      }
    }
    BroJsonValue *min = M (n, "minProperties");
    if (min && value->keys->len < bro_json_value_get_integer (min, 0))
      g_ptr_array_add (issues, invalid (path, value));
    BroJsonValue *props = M (n, "properties"), *ap = M (n, "additionalProperties");
    for (guint i = 0; i < value->keys->len; i++) {
      g_autofree char *p = join (path, value->keys->pdata[i]);
      BroJsonValue *prop = M (props, value->keys->pdata[i]);
      if (prop)
        _bro_schema_validate (value->items->pdata[i], prop, p, issues);
      else if (ap && ap->kind == BRO_JSON_VALUE_OBJECT)
        _bro_schema_validate (value->items->pdata[i], ap, p, issues);
    }
    break;
  }
  case BRO_JSON_VALUE_ARRAY: {
    BroJsonValue *items = M (n, "items");
    for (guint i = 0; items && i < value->items->len; i++) {
      g_autofree char *p = g_strdup_printf ("%s[%u]", path, i);
      _bro_schema_validate (value->items->pdata[i], items, p, issues);
    }
    break;
  }
  case BRO_JSON_VALUE_INTEGER:
  case BRO_JSON_VALUE_NUMBER: {
    double x = bro_json_value_get_number (value, 0);
    BroJsonValue *lo = M (n, "minimum"), *hi = M (n, "maximum");
    if ((lo && x < bro_json_value_get_number (lo, 0)) || (hi && x > bro_json_value_get_number (hi, 0)))
      g_ptr_array_add (issues, invalid (path, value));
    break;
  }
  case BRO_JSON_VALUE_STRING: {
    const char *pattern = bro_json_value_get_string (M (n, "pattern"), NULL);
    if (pattern) {
      g_autoptr (GRegex) re = g_regex_new (pattern, 0, 0, NULL);
      if (re && !g_regex_match (re, value->string, 0, NULL))
        g_ptr_array_add (issues, invalid (path, value));
    }
    break;
  }
  default:
    break;
  }
}
