/* bro-beta-key-page.c */
#include "bro-beta-key-page.h"

static char *
match (const char *pattern, const char *html, int group)
{
  g_autoptr (GRegex) re = g_regex_new (pattern, 0, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  return g_regex_match (re, html, 0, &m) ? g_match_info_fetch (m, group) : NULL;
}

char *
bro_beta_key_page_parse (const char *html)
{
  char *key = match ("<code>\\s*(T-[A-Za-z0-9@_]{20,})\\s*</code>", html, 1);
  return key ? key : match ("T-[A-Za-z0-9@_]{20,}", html, 0);
}
