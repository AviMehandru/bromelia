/* bro-rule.c */
#include "bro-rule.h"

#define M bro_json_value_member

static char *
opt (BroJsonValue *v)
{
  return g_strdup (bro_json_value_get_string (v, NULL));
}

static GStrv
strings (BroJsonValue *v, gboolean empty_when_missing)
{
  if (!v || v->kind != BRO_JSON_VALUE_ARRAY)
    return empty_when_missing ? g_new0 (char *, 1) : NULL;
  GStrv out = g_new0 (char *, v->items->len + 1);
  for (guint i = 0; i < v->items->len; i++)
    out[i] = g_strdup (bro_json_value_get_string (v->items->pdata[i], ""));
  return out;
}

BroRule *
bro_rule_decode (BroJsonValue *json)
{
  BroRule *r = g_new0 (BroRule, 1);
  BroJsonValue *w = M (json, "when"), *t = M (json, "then");
  r->id = g_strdup (bro_json_value_get_string (M (json, "id"), ""));
  r->name = g_strdup (bro_json_value_get_string (M (json, "name"), ""));
  r->enabled = bro_json_value_get_bool (M (json, "enabled"), TRUE);
  r->when.name_or_label = opt (M (w, "nameOrLabel"));
  r->when.name = opt (M (w, "name"));
  r->when.label = opt (M (w, "label"));
  r->when.formats = strings (M (w, "formats"), FALSE);
  r->when.kinds = strings (M (w, "kinds"), FALSE);
  r->when.drives = strings (M (w, "drives"), FALSE);
  r->when.profiles = strings (M (w, "profiles"), FALSE);
  BroJsonValue *automatic = M (w, "automatic");
  r->when.has_automatic = automatic && automatic->kind == BRO_JSON_VALUE_BOOL;
  r->when.automatic = bro_json_value_get_bool (automatic, FALSE);
  r->then.profile = opt (M (t, "profile"));
  r->then.set = M (t, "set") ? bro_json_value_ref (M (t, "set")) : NULL;
  r->then.steps = strings (M (t, "steps"), TRUE);
  return r;
}

void
bro_rule_free (BroRule *r)
{
  if (!r)
    return;
  g_free (r->id);
  g_free (r->name);
  g_free (r->when.name_or_label);
  g_free (r->when.name);
  g_free (r->when.label);
  g_strfreev (r->when.formats);
  g_strfreev (r->when.kinds);
  g_strfreev (r->when.drives);
  g_strfreev (r->when.profiles);
  g_free (r->then.profile);
  bro_json_value_unref (r->then.set);
  g_strfreev (r->then.steps);
  g_free (r);
}
