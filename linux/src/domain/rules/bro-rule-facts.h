/* bro-rule-facts.h: what rules are matched against: the movie or show name, the disc label, the format code, movie or
 * TV, the drive entry's id, the profile the disc got before rules, and whether the rip is automatic. The strings are
 * borrowed; kind, drive_id and profile_id may be NULL. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  const char *name;
  const char *label;
  const char *format_code;
  const char *kind;
  const char *drive_id;
  const char *profile_id;
  gboolean automatic;
} BroRuleFacts;

G_END_DECLS
