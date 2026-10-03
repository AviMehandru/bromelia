/* bro-identity.c */
#include "bro-identity.h"
#include "bro-format-detector.h"
#include "bro-kind-heuristics.h"
#include "bro-label-parser.h"

#include <string.h>

void
bro_identity_free (BroIdentity *identity)
{
  if (!identity)
    return;
  g_free (identity->name);
  bro_label_free (identity->label);
  bro_bro_message_free (identity->reason);
  g_free (identity);
}

static int
either (int a, int b)
{
  return a >= 0 ? a : b;
}

BroIdentity *
bro_identity_resolve (const BroIdentityInputs *inputs)
{
  const BroListing *listing = inputs->listing;
  const char *volume = listing && listing->volume_name[0] ? listing->volume_name : inputs->disc_label;
  const char *disc_name = listing ? listing->name : "";
  g_autoptr (BroLabel) from_volume = bro_label_parser_parse (volume);
  g_autoptr (BroLabel) from_name = bro_label_parser_parse (disc_name);
  g_autofree char *upper = g_utf8_strup (disc_name, -1);
  gboolean name_looks_human = disc_name[0] && !strchr (disc_name, '_') && strcmp (disc_name, volume) != 0
                              && (strchr (disc_name, ' ') || strcmp (disc_name, upper) != 0);
  BroLabel *label = bro_label_copy (from_volume);
  if (!label->title[0]) {
    g_free (label->title);
    label->title = g_strdup (from_name->title);
  }
  label->season = either (from_volume->season, from_name->season);
  label->disc = either (from_volume->disc, from_name->disc);
  label->part = either (from_volume->part, from_name->part);
  label->volume = either (from_volume->volume, from_name->volume);
  label->looks_like_series = from_volume->looks_like_series || from_name->looks_like_series;

  /* Spaces and tabs only, as on the other platforms. */
  const char *start = inputs->name_override;
  while (*start == ' ' || *start == '\t')
    start++;
  gsize len = strlen (start);
  while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
    len--;
  g_autofree char *trimmed = g_strndup (start, len);
  const char *name = trimmed;
  if (!name[0])
    name = name_looks_human && from_name->title[0] ? from_name->title : label->title;
  if (!name[0])
    name = inputs->disc_label[0] ? inputs->disc_label : "Disc";

  BroIdentity *id = g_new0 (BroIdentity, 1);
  id->name = g_strdup (name);
  id->format = inputs->has_format ? inputs->format
                                  : bro_format_detector_detect (listing, inputs->has_flags ? &inputs->flags : NULL, NULL, FALSE);
  id->format_code = bro_format_detector_code (id->format, inputs->encrypted);
  id->encrypted = inputs->encrypted;
  if (inputs->has_kind_override) {
    id->kind = inputs->kind_override;
    id->reason = bro_bro_message_new (BRO_MSG_IDENTITY_REASON_CHOSEN, NULL, BRO_SEVERITY_INFO);
  } else {
    g_autoptr (BroDecision) d = bro_kind_heuristics_decide (label, listing, inputs->play_all_episodes);
    id->kind = d->value;
    id->reason = g_steal_pointer (&d->reason);
  }
  id->label = label;
  return id;
}
