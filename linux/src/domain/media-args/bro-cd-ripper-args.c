/* bro-cd-ripper-args.c */
#include "bro-cd-ripper-args.h"

#include "bro-argument-splitter.h"
#include "bro-template-engine.h"

static GStrv
strv (const char *a, const char *b, const char *c, const char *d, const char *e)
{
  const char *items[] = { a, b, c, d, e, NULL };
  return g_strdupv ((char **) items);
}

BroCommandLine *
bro_cd_ripper_args_build (BroJsonValue *other_discs, const char *device, const char *const *available)
{
  g_autofree char *custom = g_strstrip (g_strdup (bro_json_value_get_string (bro_json_value_member (other_discs, "audioCommand"), "")));
  if (*custom) {
    g_autoptr (GHashTable) values = g_hash_table_new (g_str_hash, g_str_equal);
    g_hash_table_insert (values, (gpointer) "device", (gpointer) device);
    g_auto (GStrv) parts = bro_argument_splitter_split (custom);
    if (!parts[0])
      return NULL;
    g_autofree char *exe = bro_template_engine_render (parts[0], values);
    GPtrArray *args = g_ptr_array_new ();
    for (int i = 1; parts[i]; i++)
      g_ptr_array_add (args, bro_template_engine_render (parts[i], values));
    g_ptr_array_add (args, NULL);
    return bro_command_line_new (exe, (GStrv) g_ptr_array_free (args, FALSE));
  }
  if (available && g_strv_contains (available, "cyanrip"))
    return bro_command_line_new ("cyanrip", strv ("-d", device, "-o", "flac", NULL));
  if (available && g_strv_contains (available, "abcde"))
    return bro_command_line_new ("abcde", strv ("-d", device, "-o", "flac", "-N"));
  return NULL;
}
