/* bro-drive-join.c */
#include "bro-drive-join.h"

#include <string.h>

/* Lower case, spaces and tabs collapsed, trimmed. */
static char *
normalize (const char *s)
{
  GString *out = g_string_new (NULL);
  gboolean space = FALSE;
  for (const char *p = s; *p; p++) {
    if (*p == ' ' || *p == '\t') {
      space = out->len > 0;
      continue;
    }
    if (space)
      g_string_append_c (out, ' ');
    space = FALSE;
    g_string_append_c (out, g_ascii_tolower (*p));
  }
  return g_string_free (out, FALSE);
}

/* /dev/rdisk4 (MakeMKV on macOS) and /dev/disk4 (DiskArbitration) are one drive; E: and E:\ too. */
static char *
device_key (const char *device)
{
  char *d = g_ascii_strdown (device, -1);
  gsize n = strlen (d);
  while (n > 0 && (d[n - 1] == '\\' || d[n - 1] == '/'))
    d[--n] = '\0';
  if (g_str_has_prefix (d, "/dev/rdisk")) {
    char *k = g_strconcat ("/dev/disk", d + 10, NULL);
    g_free (d);
    return k;
  }
  return d;
}

char *
bro_drive_join_drive_id (const char *identification, const char *device)
{
  g_autofree char *name = normalize (identification ? identification : "");
  g_autofree char *key = name[0] ? g_strdup (name) : g_strconcat ("dev:", device ? device : "", NULL);
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, key, -1);
  return g_strdup_printf ("drv-%.16s", hash);
}

GPtrArray *
bro_drive_join_join (GPtrArray *makemkv_drives, GPtrArray *os_drives)
{
  GPtrArray *joined = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_joined_drive_free);
  g_autofree gboolean *used = g_new0 (gboolean, os_drives->len + 1);
  for (guint i = 0; i < makemkv_drives->len; i++) {
    const BroMakemkvDrive *m = makemkv_drives->pdata[i];
    if (!bro_makemkv_drive_is_present (m))
      continue;
    BroOsDrive *os = NULL;
    if (m->device[0]) {
      g_autofree char *key = device_key (m->device);
      for (guint k = 0; k < os_drives->len && !os; k++) {
        const BroOsDrive *o = os_drives->pdata[k];
        g_autofree char *ok = device_key (o->device);
        if (!used[k] && strcmp (ok, key) == 0) {
          used[k] = TRUE;
          os = bro_os_drive_copy (o);
        }
      }
    }
    g_autofree char *id = bro_drive_join_drive_id (m->identification, m->device);
    g_ptr_array_add (joined, bro_joined_drive_new (id, bro_makemkv_drive_copy (m), os));
  }
  for (guint k = 0; k < os_drives->len; k++) {
    if (used[k])
      continue;
    const BroOsDrive *o = os_drives->pdata[k];
    g_autofree char *id = bro_drive_join_drive_id ("", o->device);
    g_ptr_array_add (joined, bro_joined_drive_new (id, NULL, bro_os_drive_copy (o)));
  }
  return joined;
}

gboolean
bro_drive_join_matches (const BroDriveMatch *match, const BroMakemkvDrive *drive)
{
  g_autofree char *name = normalize (match->drive_name);
  if (name[0]) {
    g_autofree char *other = normalize (drive->identification);
    return strcmp (name, other) == 0;
  }
  return match->device_path[0] && g_ascii_strcasecmp (match->device_path, drive->device) == 0;
}

BroDriveEntry *
bro_drive_join_entry_for (GPtrArray *drives, const BroMakemkvDrive *drive)
{
  for (guint i = 0; i < drives->len; i++) {
    BroDriveEntry *e = drives->pdata[i];
    if (e->enabled && bro_drive_join_matches (e->match, drive))
      return e;
  }
  for (guint i = 0; i < drives->len; i++) {
    BroDriveEntry *e = drives->pdata[i];
    if (bro_drive_join_matches (e->match, drive))
      return e;
  }
  return NULL;
}

char *
bro_drive_join_short_model (const char *identification)
{
  g_auto (GStrv) parts = g_strsplit (identification, " ", -1);
  GPtrArray *words = g_ptr_array_new ();
  for (int i = 0; parts[i]; i++)
    if (parts[i][0])
      g_ptr_array_add (words, parts[i]);
  char *out;
  if (words->len > 2) {
    GString *s = g_string_new (NULL);
    for (guint i = 1; i < words->len && i <= 3; i++) {
      if (s->len)
        g_string_append_c (s, ' ');
      g_string_append (s, words->pdata[i]);
    }
    out = g_string_free (s, FALSE);
  } else {
    out = g_strdup (identification[0] ? identification : "Drive");
  }
  g_ptr_array_unref (words);
  return out;
}
