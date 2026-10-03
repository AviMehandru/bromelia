/* bro-config-validator.c */
#include "bro-config-validator.h"
#include "bro-config-private.h"

#include <string.h>

#define M bro_json_value_member

typedef struct {
  GPtrArray *issues;
  GHashTable *ids[6]; /* libraries, profiles, drives, steps, targets, rules */
} Check;

static const char *const kinds[] = { "libraries", "profiles", "drives", "steps", "targets", "rules" };

static void
add (Check *c, const char *path, BroMessageCode code, BroJsonValue *params)
{
  g_ptr_array_add (c->issues, bro_issue_new (path, code, params, BRO_SEVERITY_ERROR));
}

static void
regex (Check *c, BroJsonValue *v, const char *path)
{
  const char *pattern = bro_json_value_get_string (v, NULL);
  if (!pattern || !pattern[0])
    return;
  g_autoptr (GRegex) re = g_regex_new (pattern, G_REGEX_CASELESS, 0, NULL);
  if (!re)
    add (c, path, BRO_MSG_CONFIG_INVALID_REGEX, NULL);
}

static void
titles (Check *c, BroJsonValue *profile, const char *path)
{
  g_autofree char *inc = g_strconcat (path, ".titles.includePattern", NULL);
  g_autofree char *exc = g_strconcat (path, ".titles.excludePattern", NULL);
  regex (c, M (M (profile, "titles"), "includePattern"), inc);
  regex (c, M (M (profile, "titles"), "excludePattern"), exc);
}

static void
need (Check *c, int kind, const char *noun, BroJsonValue *v, const char *path)
{
  const char *id = bro_json_value_get_string (v, NULL);
  if (!id || g_hash_table_contains (c->ids[kind], id))
    return;
  BroJsonValue *p = bro_json_value_new_object ();
  bro_json_value_set (p, "kind", bro_json_value_new_string (noun));
  bro_json_value_set (p, "id", bro_json_value_new_string (id));
  add (c, path, BRO_MSG_CONFIG_UNKNOWN_REFERENCE, p);
}

static void
need_all (Check *c, int kind, const char *noun, BroJsonValue *list, const char *path)
{
  for (guint i = 0; i < bro_json_value_length (list); i++) {
    g_autofree char *p = g_strdup_printf ("%s[%u]", path, i);
    need (c, kind, noun, bro_json_value_at (list, i), p);
  }
}

GPtrArray *
bro_config_validator_validate (const BroConfig *config)
{
  Check c = { g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free), { NULL } };
  for (guint i = 0; i < config->issues->len; i++)
    g_ptr_array_add (c.issues, bro_issue_copy (config->issues->pdata[i]));
  BroJsonValue *doc = config->document;
  _bro_schema_validate (doc, _bro_schema_document (), "", c.issues);
  for (int k = 0; k < 6; k++) {
    c.ids[k] = g_hash_table_new (g_str_hash, g_str_equal);
    BroJsonValue *list = M (doc, kinds[k]);
    for (guint i = 0; i < bro_json_value_length (list); i++) {
      const char *id = bro_json_value_get_string (M (bro_json_value_at (list, i), "id"), NULL);
      if (id && !g_hash_table_add (c.ids[k], (gpointer) id)) {
        g_autofree char *p = g_strdup_printf ("%s[%u].id", kinds[k], i);
        BroJsonValue *params = bro_json_value_new_object ();
        bro_json_value_set (params, "id", bro_json_value_new_string (id));
        add (&c, p, BRO_MSG_CONFIG_DUPLICATE_ID, params);
      }
    }
  }
  enum { LIBRARIES, PROFILES, DRIVES, STEPS, TARGETS };
  need (&c, LIBRARIES, "library", M (doc, "defaultLibrary"), "defaultLibrary");
  need (&c, PROFILES, "profile", M (doc, "defaultProfile"), "defaultProfile");
  BroJsonValue *profiles = M (doc, "profiles");
  for (guint i = 0; i < bro_json_value_length (profiles); i++) {
    BroJsonValue *p = bro_json_value_at (profiles, i);
    g_autofree char *base = g_strdup_printf ("profiles[%u]", i);
    g_autofree char *lib = g_strconcat (base, ".library", NULL);
    g_autofree char *steps = g_strconcat (base, ".steps", NULL);
    g_autofree char *targets = g_strconcat (base, ".archive.replicateTo", NULL);
    need (&c, LIBRARIES, "library", M (p, "library"), lib);
    need_all (&c, STEPS, "step", M (p, "steps"), steps);
    need_all (&c, TARGETS, "target", M (M (p, "archive"), "replicateTo"), targets);
    titles (&c, p, base);
  }
  BroJsonValue *drives = M (doc, "drives");
  for (guint i = 0; i < bro_json_value_length (drives); i++) {
    g_autofree char *p = g_strdup_printf ("drives[%u].profile", i);
    need (&c, PROFILES, "profile", M (bro_json_value_at (drives, i), "profile"), p);
  }
  BroJsonValue *rules = M (doc, "rules");
  for (guint i = 0; i < bro_json_value_length (rules); i++) {
    BroJsonValue *r = bro_json_value_at (rules, i);
    const char *keys[] = { "nameOrLabel", "name", "label" };
    for (int k = 0; k < 3; k++) {
      g_autofree char *p = g_strdup_printf ("rules[%u].when.%s", i, keys[k]);
      regex (&c, M (M (r, "when"), keys[k]), p);
    }
    g_autofree char *wp = g_strdup_printf ("rules[%u].when.profiles", i);
    g_autofree char *wd = g_strdup_printf ("rules[%u].when.drives", i);
    g_autofree char *tp = g_strdup_printf ("rules[%u].then.profile", i);
    g_autofree char *ts = g_strdup_printf ("rules[%u].then.steps", i);
    g_autofree char *set = g_strdup_printf ("rules[%u].then.set", i);
    need_all (&c, PROFILES, "profile", M (M (r, "when"), "profiles"), wp);
    need_all (&c, DRIVES, "drive", M (M (r, "when"), "drives"), wd);
    need (&c, PROFILES, "profile", M (M (r, "then"), "profile"), tp);
    need_all (&c, STEPS, "step", M (M (r, "then"), "steps"), ts);
    titles (&c, M (M (r, "then"), "set"), set);
  }
  BroJsonValue *server = M (doc, "server"), *tcp = M (server, "tcp"), *tls = M (server, "tls");
  const char *address = bro_json_value_get_string (M (tcp, "address"), NULL);
  if (bro_json_value_get_bool (M (tcp, "enabled"), FALSE) && address && strcmp (address, "127.0.0.1") != 0
      && bro_json_value_length (M (server, "tokens")) == 0)
    add (&c, "server.tcp.address", BRO_MSG_CONFIG_NETWORK_NEEDS_TOKEN, NULL);
  gboolean cert = bro_json_value_get_string (M (tls, "certificate"), "")[0] != '\0';
  gboolean key = bro_json_value_get_string (M (tls, "key"), "")[0] != '\0';
  if (cert != key)
    add (&c, "server.tls", BRO_MSG_CONFIG_TLS_NEEDS_BOTH, NULL);
  for (int k = 0; k < 6; k++)
    g_hash_table_unref (c.ids[k]);
  return c.issues;
}
