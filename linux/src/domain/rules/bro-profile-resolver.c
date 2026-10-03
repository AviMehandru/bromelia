/* bro-profile-resolver.c */
#include "bro-profile-resolver.h"

#include "bro-config-private.h"
#include "bro-rule-matcher.h"

#include <string.h>

#define M bro_json_value_member

/* A profile as a patch: without its id and name. */
static BroJsonValue *
strip (BroJsonValue *p)
{
  BroJsonValue *out = bro_json_value_new_object ();
  for (guint i = 0; p && p->kind == BRO_JSON_VALUE_OBJECT && i < p->keys->len; i++)
    if (strcmp (p->keys->pdata[i], "id") != 0 && strcmp (p->keys->pdata[i], "name") != 0)
      bro_json_value_set (out, p->keys->pdata[i], bro_json_value_ref (p->items->pdata[i]));
  return out;
}

/* RFC 7386: objects merge key by key, null removes a key, anything else replaces. */
static BroJsonValue *
merge_patch (BroJsonValue *target, BroJsonValue *patch)
{
  if (patch->kind != BRO_JSON_VALUE_OBJECT)
    return bro_json_value_ref (patch);
  BroJsonValue *out = bro_json_value_new_object ();
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  if (target->kind == BRO_JSON_VALUE_OBJECT)
    for (guint i = 0; i < target->keys->len; i++) {
      BroJsonValue *p = M (patch, target->keys->pdata[i]);
      if (!p)
        bro_json_value_set (out, target->keys->pdata[i], bro_json_value_ref (target->items->pdata[i]));
      else if (p->kind != BRO_JSON_VALUE_NULL)
        bro_json_value_set (out, target->keys->pdata[i], merge_patch (target->items->pdata[i], p));
    }
  for (guint i = 0; i < patch->keys->len; i++) {
    BroJsonValue *p = patch->items->pdata[i];
    if (p->kind != BRO_JSON_VALUE_NULL && !M (target, patch->keys->pdata[i]))
      bro_json_value_set (out, patch->keys->pdata[i], merge_patch (empty, p));
  }
  return out;
}

static void
leaf_keys (BroJsonValue *v, const char *prefix, GPtrArray *keys)
{
  for (guint i = 0; v->kind == BRO_JSON_VALUE_OBJECT && i < v->keys->len; i++) {
    BroJsonValue *child = v->items->pdata[i];
    char *path = *prefix ? g_strconcat (prefix, ".", v->keys->pdata[i], NULL) : g_strdup (v->keys->pdata[i]);
    if (child->kind == BRO_JSON_VALUE_OBJECT && child->keys->len > 0) {
      leaf_keys (child, path, keys);
      g_free (path);
    } else {
      g_ptr_array_add (keys, path);
    }
  }
}

static void
layer (BroJsonValue **profile, GPtrArray *trace, BroResolveLayer l, const char *id, BroJsonValue *patch)
{
  g_autoptr (BroJsonValue) p = strip (patch);
  BroJsonValue *merged = merge_patch (*profile, p);
  bro_json_value_unref (*profile);
  *profile = merged;
  GPtrArray *keys = g_ptr_array_new ();
  leaf_keys (p, "", keys);
  g_ptr_array_add (keys, NULL);
  g_ptr_array_add (trace, bro_resolve_trace_new (l, id, (GStrv) g_ptr_array_free (keys, FALSE)));
}

static BroJsonValue *
profile_json (GPtrArray *profiles, const char *id)
{
  for (guint i = 0; i < profiles->len; i++) {
    BroProfile *p = profiles->pdata[i];
    if (g_strcmp0 (bro_json_value_get_string (M (p->json, "id"), ""), id) == 0)
      return p->json;
  }
  return NULL;
}

BroEffectiveProfile *
bro_profile_resolver_resolve (const BroConfig *config, const char *drive_id, const BroRuleFacts *facts, BroJsonValue *session_choices)
{
  BroJsonValue *doc = config->document;
  g_autoptr (GPtrArray) profiles = bro_config_profiles (config);
  g_autoptr (GPtrArray) drives = bro_config_drives (config);
  g_autoptr (GPtrArray) rules = bro_config_rules (config);
  const char *default_id = bro_json_value_get_string (M (doc, "defaultProfile"), "default");
  BroDriveEntry *drive = NULL;
  for (guint i = 0; drive_id && i < drives->len && !drive; i++)
    if (strcmp (((BroDriveEntry *) drives->pdata[i])->id, drive_id) == 0)
      drive = drives->pdata[i];
  const char *drive_profile_id = drive && drive->profile && strcmp (drive->profile, default_id) != 0 ? drive->profile : NULL;

  g_autoptr (GPtrArray) rule_layers = g_ptr_array_new (); /* BroRule *, borrowed */
  g_autoptr (GPtrArray) rule_steps = g_ptr_array_new ();  /* char *, borrowed */
  const char *current = drive_profile_id ? drive_profile_id : default_id;
  for (guint i = 0; i < rules->len; i++) {
    BroRule *rule = rules->pdata[i];
    if (!rule->enabled)
      continue;
    BroRuleFacts f = *facts;
    f.drive_id = drive_id;
    f.profile_id = current;
    if (!bro_rule_matcher_matches (&rule->when, &f))
      continue;
    if (rule->then.profile) {
      drive_profile_id = strcmp (rule->then.profile, default_id) == 0 ? NULL : rule->then.profile;
      current = rule->then.profile;
    }
    g_ptr_array_add (rule_layers, rule);
    for (int k = 0; rule->then.steps[k]; k++)
      g_ptr_array_add (rule_steps, rule->then.steps[k]);
  }

  GPtrArray *trace = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_resolve_trace_free);
  g_autoptr (GPtrArray) issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  BroJsonValue *node = _bro_schema_def ("ProfileFields");
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  BroJsonValue *profile = _bro_schema_normalize_full (empty, node, "", TRUE, TRUE, issues);
  g_ptr_array_add (trace, bro_resolve_trace_new (BRO_RESOLVE_LAYER_DEFAULTS, NULL, NULL));
  layer (&profile, trace, BRO_RESOLVE_LAYER_DEFAULT_PROFILE, default_id, profile_json (profiles, default_id));
  if (drive_profile_id)
    layer (&profile, trace, BRO_RESOLVE_LAYER_DRIVE_PROFILE, drive_profile_id, profile_json (profiles, drive_profile_id));
  if (drive) {
    /* In the document's order (the entry's hash table has none). */
    BroJsonValue *settings = NULL;
    BroJsonValue *list = M (doc, "drives");
    for (guint i = 0; i < bro_json_value_length (list) && !settings; i++)
      if (g_strcmp0 (bro_json_value_get_string (M (bro_json_value_at (list, i), "id"), NULL), drive->id) == 0)
        settings = M (bro_json_value_at (list, i), "makemkvSettings");
    g_autoptr (BroJsonValue) patch = bro_json_value_new_object ();
    if (settings && settings->kind == BRO_JSON_VALUE_OBJECT && settings->keys->len > 0) {
      BroJsonValue *makemkv = bro_json_value_new_object ();
      bro_json_value_set (makemkv, "settings", bro_json_value_ref (settings));
      bro_json_value_set (patch, "makemkv", makemkv);
    }
    layer (&profile, trace, BRO_RESOLVE_LAYER_DRIVE_OVERRIDES, drive->id, patch);
  }
  for (guint i = 0; i < rule_layers->len; i++) {
    BroRule *rule = rule_layers->pdata[i];
    layer (&profile, trace, BRO_RESOLVE_LAYER_RULE, rule->id, rule->then.set);
  }
  if (session_choices)
    layer (&profile, trace, BRO_RESOLVE_LAYER_SESSION, NULL, session_choices);
  /* A key a patch removed takes its default again. */
  BroJsonValue *full = _bro_schema_normalize_full (profile, node, "", TRUE, TRUE, issues);
  bro_json_value_unref (profile);

  GPtrArray *steps = g_ptr_array_new ();
  BroJsonValue *own = M (full, "steps");
  for (guint i = 0; i < bro_json_value_length (own) + rule_steps->len; i++) {
    const char *s = i < bro_json_value_length (own) ? bro_json_value_get_string (bro_json_value_at (own, i), "")
                                                     : rule_steps->pdata[i - bro_json_value_length (own)];
    gboolean seen = FALSE;
    for (guint k = 0; k < steps->len && !seen; k++)
      seen = strcmp (steps->pdata[k], s) == 0;
    if (!seen)
      g_ptr_array_add (steps, g_strdup (s));
  }
  g_ptr_array_add (steps, NULL);
  return bro_effective_profile_new (full, (GStrv) g_ptr_array_free (steps, FALSE), trace);
}
