/* bro-medium-dto.c */
#include "bro-medium-dto.h"

#include "bro-format-detector.h"

GPtrArray *
bro_medium_dto_flag_names (BroDiscFlags flags)
{
  static const struct {
    int bit;
    const char *name;
  } bits[] = {
    { BRO_DISC_FLAGS_DVD_FILES, "dvdFiles" },     { BRO_DISC_FLAGS_HD_DVD_FILES, "hdDvdFiles" }, { BRO_DISC_FLAGS_BLURAY_FILES, "blurayFiles" },
    { BRO_DISC_FLAGS_AACS_FILES, "aacsFiles" },   { BRO_DISC_FLAGS_BDSVM_FILES, "bdsvmFiles" },
  };
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < G_N_ELEMENTS (bits); i++)
    if (flags.raw & bits[i].bit)
      g_ptr_array_add (names, g_strdup (bits[i].name));
  return names;
}

const char *
bro_medium_dto_type_of (BroDiscFlags flags)
{
  switch (bro_format_detector_detect (NULL, &flags, NULL, FALSE))
    {
    case BRO_DISC_FORMAT_BLURAY:
    case BRO_DISC_FORMAT_UHD:
      return "bd";
    case BRO_DISC_FORMAT_HDDVD:
      return "hddvd";
    case BRO_DISC_FORMAT_DVD:
      return "dvd";
    default:
      return "disc";
    }
}

BroMediumDto *
bro_medium_dto_new (gint64 generation, const char *label, BroDiscFlags raw_flags, BroDiscContent content, const char *mount_path)
{
  BroMediumDto *m = g_new0 (BroMediumDto, 1);
  m->generation = generation;
  m->label = g_strdup (label);
  m->type = g_strdup (bro_medium_dto_type_of (raw_flags));
  m->type_text = g_strdup (bro_disc_flags_type_text (raw_flags));
  m->flags = bro_medium_dto_flag_names (raw_flags);
  m->content = content;
  m->mount_path = g_strdup (mount_path);
  return m;
}

BroMediumDto *
bro_medium_dto_copy (const BroMediumDto *m)
{
  BroMediumDto *c;
  if (!m)
    return NULL;
  c = g_new0 (BroMediumDto, 1);
  c->generation = m->generation;
  c->label = g_strdup (m->label);
  c->type = g_strdup (m->type);
  c->type_text = g_strdup (m->type_text);
  c->flags = g_ptr_array_copy (m->flags, (GCopyFunc) (void (*) (void)) g_strdup, NULL);
  c->content = m->content;
  c->mount_path = g_strdup (m->mount_path);
  return c;
}

void
bro_medium_dto_free (BroMediumDto *m)
{
  if (!m)
    return;
  g_free (m->label);
  g_free (m->type);
  g_free (m->type_text);
  g_ptr_array_unref (m->flags);
  g_free (m->mount_path);
  g_free (m);
}

BroJsonValue *
bro_medium_dto_json (const BroMediumDto *m)
{
  BroJsonValue *o = bro_json_value_new_object (), *flags = bro_json_value_new_array ();
  bro_json_value_set (o, "generation", bro_json_value_new_integer (m->generation));
  bro_json_value_set (o, "label", bro_json_value_new_string (m->label));
  bro_json_value_set (o, "type", bro_json_value_new_string (m->type));
  bro_json_value_set (o, "typeText", bro_json_value_new_string (m->type_text));
  for (guint i = 0; i < m->flags->len; i++)
    bro_json_value_append (flags, bro_json_value_new_string (m->flags->pdata[i]));
  bro_json_value_set (o, "flags", flags);
  bro_json_value_set (o, "content", bro_json_value_new_string (bro_disc_content_to_wire (m->content)));
  bro_json_value_set (o, "mountPath", m->mount_path ? bro_json_value_new_string (m->mount_path) : bro_json_value_new_null ());
  return o;
}
