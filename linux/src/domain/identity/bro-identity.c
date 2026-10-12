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

static BroJsonValue *
part_json (int v)
{
  return v >= 0 ? bro_json_value_new_integer (v) : bro_json_value_new_null ();
}

BroJsonValue *
bro_identity_encode (const BroIdentity *identity)
{
  const BroLabel *l = identity->label;
  BroJsonValue *label = bro_json_value_new_object ();
  bro_json_value_set (label, "title", bro_json_value_new_string (l->title));
  bro_json_value_set (label, "season", part_json (l->season));
  bro_json_value_set (label, "part", part_json (l->part));
  bro_json_value_set (label, "volume", part_json (l->volume));
  bro_json_value_set (label, "disc", part_json (l->disc));
  g_autofree char *set = bro_label_parser_set_description (l);
  bro_json_value_set (label, "set", bro_json_value_new_string (set));
  bro_json_value_set (label, "looksLikeSeries", bro_json_value_new_bool (l->looks_like_series));
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "name", bro_json_value_new_string (identity->name));
  bro_json_value_set (o, "kind", bro_json_value_new_string (bro_media_kind_to_wire (identity->kind)));
  bro_json_value_set (o, "format", bro_json_value_new_string (bro_disc_format_to_wire (identity->format)));
  bro_json_value_set (o, "formatCode", bro_json_value_new_string (identity->format_code.text));
  bro_json_value_set (o, "encrypted", bro_json_value_new_bool (identity->encrypted));
  bro_json_value_set (o, "label", label);
  bro_json_value_set (o, "reason", bro_bro_message_to_json (identity->reason));
  return o;
}

static int
part_of (const BroJsonValue *label, const char *key)
{
  BroJsonValue *v = bro_json_value_member (label, key);
  return v && (v->kind == BRO_JSON_VALUE_INTEGER || v->kind == BRO_JSON_VALUE_NUMBER) ? (int) bro_json_value_get_integer (v, -1) : -1;
}

BroIdentity *
bro_identity_decode (const BroJsonValue *json)
{
  BroIdentity *id = g_new0 (BroIdentity, 1);
  id->name = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "name"), ""));
  if (!bro_media_kind_from_wire (bro_json_value_get_string (bro_json_value_member (json, "kind"), ""), &id->kind))
    id->kind = BRO_MEDIA_KIND_MOVIE;
  if (!bro_disc_format_from_wire (bro_json_value_get_string (bro_json_value_member (json, "format"), ""), &id->format))
    id->format = BRO_DISC_FORMAT_UNKNOWN;
  g_strlcpy (id->format_code.text, bro_json_value_get_string (bro_json_value_member (json, "formatCode"), ""), sizeof id->format_code.text);
  id->encrypted = bro_json_value_get_bool (bro_json_value_member (json, "encrypted"), FALSE);
  const BroJsonValue *l = bro_json_value_member (json, "label");
  id->label = g_new0 (BroLabel, 1);
  id->label->title = g_strdup (bro_json_value_get_string (bro_json_value_member (l, "title"), ""));
  id->label->season = part_of (l, "season");
  id->label->part = part_of (l, "part");
  id->label->volume = part_of (l, "volume");
  id->label->disc = part_of (l, "disc");
  id->label->looks_like_series = bro_json_value_get_bool (bro_json_value_member (l, "looksLikeSeries"), FALSE);
  id->reason = bro_bro_message_decode (bro_json_value_member (json, "reason"));
  return id;
}
