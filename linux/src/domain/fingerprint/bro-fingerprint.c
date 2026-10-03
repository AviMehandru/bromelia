/* bro-fingerprint.c */
#include "bro-fingerprint.h"

#include <string.h>

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

char *
bro_fingerprint_of (const BroListing *listing)
{
  if (!listing || listing->titles->len == 0)
    return NULL;
  const char *volume = listing->volume_name[0] ? listing->volume_name : listing->name;
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < listing->titles->len; i++) {
    const BroTitle *t = listing->titles->pdata[i];
    g_ptr_array_add (lines, g_strdup_printf ("%d|%d|%s|%" G_GINT64_FORMAT, t->has_source_title_id ? t->source_title_id : -1,
                                             t->duration_seconds, t->segment_map, t->size_bytes));
  }
  g_ptr_array_sort (lines, compare_strings);
  g_autoptr (GString) text = g_string_new (NULL);
  g_string_append_printf (text, "bromelia-disc-fingerprint 1\nvolume:%s\ntitles:%u\n", volume, listing->titles->len);
  for (guint i = 0; i < lines->len; i++)
    g_string_append_printf (text, "%s\n", (char *) lines->pdata[i]);
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, text->str, text->len);
  return g_strdup_printf ("v1:%.32s", hash);
}
