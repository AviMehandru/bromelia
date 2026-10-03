/* bro-verify-result.c */
#include "bro-verify-result.h"
#include "bro-archive-files.h"

#include <string.h>

void
bro_verify_result_free (BroVerifyResult *r)
{
  if (!r)
    return;
  g_strfreev (r->changed);
  g_strfreev (r->unreadable);
  g_strfreev (r->missing);
  g_strfreev (r->unlisted);
  bro_bro_message_free (r->summary);
  g_free (r);
}

static int
by_bytes (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char **) a, *(const char **) b);
}

static GStrv
to_strv (GPtrArray *a)
{
  GPtrArray *c = g_ptr_array_new ();
  for (guint i = 0; i < a->len; i++)
    g_ptr_array_add (c, g_strdup (a->pdata[i]));
  g_ptr_array_add (c, NULL);
  return (GStrv) g_ptr_array_free (c, FALSE);
}

static BroJsonValue *
count_part (BroMessageCode code, guint count)
{
  BroJsonValue *p = bro_json_value_new_object ();
  bro_json_value_set (p, "count", bro_json_value_new_integer (count));
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, p, BRO_SEVERITY_INFO);
  return bro_bro_message_to_json (m);
}

BroVerifyResult *
bro_verify_result_compare (const char *const *sums, GHashTable *hashes, const char *const *folder_files)
{
  BroVerifyResult *r = g_new0 (BroVerifyResult, 1);
  g_autoptr (GHashTable) set = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) listed = g_ptr_array_new ();
  for (int i = 0; sums && sums[i]; i++)
    if (g_hash_table_add (set, (gpointer) sums[i]))
      g_ptr_array_add (listed, (gpointer) sums[i]);
  g_ptr_array_sort (listed, by_bytes);
  g_autoptr (GPtrArray) changed = g_ptr_array_new (), unreadable = g_ptr_array_new (), missing = g_ptr_array_new (),
                        unlisted = g_ptr_array_new ();
  for (guint i = 0; i < listed->len; i++) {
    gpointer v;
    BroFileVerdict verdict = g_hash_table_lookup_extended (hashes, listed->pdata[i], NULL, &v) ? (BroFileVerdict) GPOINTER_TO_INT (v)
                                                                                                : BRO_FILE_VERDICT_MISSING;
    if (verdict == BRO_FILE_VERDICT_CHANGED)
      g_ptr_array_add (changed, listed->pdata[i]);
    else if (verdict == BRO_FILE_VERDICT_UNREADABLE)
      g_ptr_array_add (unreadable, listed->pdata[i]);
    else if (verdict == BRO_FILE_VERDICT_MISSING)
      g_ptr_array_add (missing, listed->pdata[i]);
  }
  for (int i = 0; folder_files && folder_files[i]; i++) {
    const char *f = folder_files[i];
    const char *slash = strrchr (f, '/');
    const char *name = slash ? slash + 1 : f;
    gboolean hidden = f[0] == '.' || strstr (f, "/.");
    if (!g_hash_table_contains (set, f) && !(!slash && bro_archive_files_is_own_file (name)) && !bro_archive_files_is_metadata_file (name)
        && !hidden)
      g_ptr_array_add (unlisted, (gpointer) f);
  }
  g_ptr_array_sort (unlisted, by_bytes);
  r->files = listed->len;
  r->changed = to_strv (changed);
  r->unreadable = to_strv (unreadable);
  r->missing = to_strv (missing);
  r->unlisted = to_strv (unlisted);
  if (listed->len == 0) {
    r->result = BRO_CHECK_RESULT_ERROR;
    r->summary = bro_bro_message_new (BRO_MSG_CHECK_SUMS_EMPTY, NULL, BRO_SEVERITY_ERROR);
    return r;
  }
  BroJsonValue *p = bro_json_value_new_object ();
  if (changed->len == 0 && unreadable->len == 0 && missing->len == 0) {
    r->result = BRO_CHECK_RESULT_OK;
    bro_json_value_set (p, "files", bro_json_value_new_integer (listed->len));
    bro_json_value_set (p, "unlisted", bro_json_value_new_integer (unlisted->len));
    r->summary = bro_bro_message_new (BRO_MSG_CHECK_OK, p, BRO_SEVERITY_INFO);
  } else {
    r->result = BRO_CHECK_RESULT_DAMAGED;
    BroJsonValue *parts = bro_json_value_new_array ();
    if (changed->len)
      bro_json_value_append (parts, count_part (BRO_MSG_CHECK_PART_CHANGED, changed->len));
    if (unreadable->len)
      bro_json_value_append (parts, count_part (BRO_MSG_CHECK_PART_UNREADABLE, unreadable->len));
    if (missing->len)
      bro_json_value_append (parts, count_part (BRO_MSG_CHECK_PART_MISSING, missing->len));
    bro_json_value_set (p, "parts", parts);
    bro_json_value_set (p, "files", bro_json_value_new_integer (listed->len));
    bro_json_value_set (p, "unlisted", bro_json_value_new_integer (unlisted->len));
    r->summary = bro_bro_message_new (BRO_MSG_CHECK_DAMAGED, p, BRO_SEVERITY_ERROR);
  }
  return r;
}
