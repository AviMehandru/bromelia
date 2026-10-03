/* bro-title.c */
#include "bro-title.h"

void
bro_title_free (BroTitle *title)
{
  if (!title)
    return;
  g_free (title->source_file);
  g_free (title->name);
  g_free (title->comment);
  g_free (title->duration);
  g_free (title->segment_map);
  g_free (title->output_file_name);
  g_ptr_array_unref (title->tracks);
  g_hash_table_unref (title->attributes);
  g_free (title);
}
