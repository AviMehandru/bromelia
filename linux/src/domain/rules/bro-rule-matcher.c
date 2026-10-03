/* bro-rule-matcher.c */
#include "bro-rule-matcher.h"

#include <string.h>

static gboolean
search (const char *pattern, const char *text)
{
  g_autoptr (GRegex) re = g_regex_new (pattern, G_REGEX_CASELESS, 0, NULL);
  return re && g_regex_match (re, text ? text : "", 0, NULL);
}

static gboolean
format_matches (const char *format, const char *code)
{
  g_autofree char *f = g_ascii_strdown (format, -1);
  g_strstrip (f);
  g_autofree char *c = g_ascii_strdown (code ? code : "", -1);
  gsize n = strlen (f);
  if (n > 0 && f[n - 1] == '*')
    return strncmp (c, f, n - 1) == 0;
  return strcmp (c, f) == 0;
}

static gboolean
has (GStrv list, const char *value)
{
  return g_strv_contains ((const char *const *) list, value ? value : "");
}

gboolean
bro_rule_matcher_matches (const BroRuleWhen *when, const BroRuleFacts *facts)
{
  if (when->name_or_label && *when->name_or_label && !(search (when->name_or_label, facts->name) || search (when->name_or_label, facts->label)))
    return FALSE;
  if (when->name && *when->name && !search (when->name, facts->name))
    return FALSE;
  if (when->label && *when->label && !search (when->label, facts->label))
    return FALSE;
  if (when->formats && when->formats[0]) {
    gboolean any = FALSE;
    for (int i = 0; when->formats[i] && !any; i++)
      any = format_matches (when->formats[i], facts->format_code);
    if (!any)
      return FALSE;
  }
  if (when->kinds && when->kinds[0] && !has (when->kinds, facts->kind))
    return FALSE;
  if (when->drives && when->drives[0] && !has (when->drives, facts->drive_id))
    return FALSE;
  if (when->profiles && when->profiles[0] && !has (when->profiles, facts->profile_id))
    return FALSE;
  if (when->has_automatic && !when->automatic != !facts->automatic)
    return FALSE;
  return TRUE;
}
