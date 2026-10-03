/* bro-rule-when.h: a rule's conditions; every condition given must hold, and an empty one matches every disc:
 * regular expressions on the name and label, format codes (a trailing * matches a prefix), kinds, drive entries,
 * the profile before rules, and automatic or not. Part of BroRule; NULL = not given. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *name_or_label;
  char *name;
  char *label;
  GStrv formats;
  GStrv kinds;
  GStrv drives;
  GStrv profiles;
  gboolean has_automatic;
  gboolean automatic;
} BroRuleWhen;

G_END_DECLS
