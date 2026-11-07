/* bro-sha256-sums.c */
#include "bro-sha256-sums.h"

#include <string.h>

GPtrArray *
bro_sha256_sums_parse (const char *text)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_sum_entry_free);
  g_autoptr (GRegex) re = g_regex_new ("^([0-9a-fA-F]{64}) [ *](.+)$", 0, 0, NULL);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    gsize n = strlen (lines[i]);
    if (n && lines[i][n - 1] == '\r')
      lines[i][n - 1] = '\0';
    g_autoptr (GMatchInfo) m = NULL;
    if (!g_regex_match (re, lines[i], 0, &m))
      continue;
    g_autofree char *hash = g_match_info_fetch (m, 1);
    g_autofree char *path = g_match_info_fetch (m, 2);
    g_autofree char *lower = g_ascii_strdown (hash, -1);
    g_ptr_array_add (out, bro_sum_entry_new (path, lower));
  }
  return out;
}

static int
by_path (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroSumEntry **) a)->path, (*(BroSumEntry **) b)->path);
}

char *
bro_sha256_sums_render (GPtrArray *entries)
{
  g_autoptr (GPtrArray) sorted = g_ptr_array_new ();
  g_ptr_array_extend (sorted, entries, NULL, NULL);
  g_ptr_array_sort (sorted, by_path);
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < sorted->len; i++) {
    const BroSumEntry *e = sorted->pdata[i];
    g_string_append_printf (out, "%s  %s\n", e->sha256, e->path);
  }
  return g_string_free (out, FALSE);
}

GBytes *
bro_sha256_sums_merge (GBytes *existing, GPtrArray *entries)
{
  g_autoptr (GHashTable) merged = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (GByteArray) unread = g_byte_array_new ();
  gsize size;
  const char *data = g_bytes_get_data (existing, &size);
  for (gsize start = 0, end; start < size; start = end + 1) {
    const char *next = memchr (data + start, '\n', size - start);
    gsize n;
    end = next ? (gsize) (next - data) : size;
    n = end - start;
    if (n && data[end - 1] == '\r')
      n--;
    if (!n)
      continue;
    g_autofree char *line = g_strndup (data + start, n);
    g_autoptr (GPtrArray) parsed = strlen (line) == n && g_utf8_validate (line, -1, NULL) ? bro_sha256_sums_parse (line) : NULL;
    if (parsed && parsed->len) {
      const BroSumEntry *e = parsed->pdata[0];
      g_hash_table_insert (merged, g_strdup (e->path), g_strdup (e->sha256));
    } else {
      g_byte_array_append (unread, (const guint8 *) data + start, n);
      g_byte_array_append (unread, (const guint8 *) "\n", 1);
    }
  }
  for (guint i = 0; i < entries->len; i++) {
    const BroSumEntry *e = entries->pdata[i];
    g_hash_table_insert (merged, g_strdup (e->path), g_strdup (e->sha256));
  }
  g_autoptr (GPtrArray) all = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_sum_entry_free);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, merged);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_ptr_array_add (all, bro_sum_entry_new (k, v));
  g_autofree char *text = bro_sha256_sums_render (all);
  GByteArray *out = g_byte_array_new ();
  g_byte_array_append (out, (const guint8 *) text, strlen (text));
  g_byte_array_append (out, unread->data, unread->len);
  return g_byte_array_free_to_bytes (out);
}

char *
bro_sha256_sums_hash (const guint8 *bytes, gsize length)
{
  return g_compute_checksum_for_data (G_CHECKSUM_SHA256, bytes, length);
}
