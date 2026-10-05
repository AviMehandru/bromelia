/* bro-home-dir-isolation.c */
#include "bro-home-dir-isolation.h"

#include "bro-settings-conf.h"
#include <string.h>

/* ---- the lease ---- */

#define BRO_TYPE_HOME_LEASE (bro_home_lease_get_type ())
G_DECLARE_FINAL_TYPE (BroHomeLease, bro_home_lease, BRO, HOME_LEASE, GObject)

struct _BroHomeLease {
  GObject parent_instance;
  BroFileSystem *fs;
  char *home;
  char *conf;
  char *profile; /* nullable */
};

static void bro_home_lease_iface_init (BroIsolationLeaseInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroHomeLease, bro_home_lease, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_ISOLATION_LEASE, bro_home_lease_iface_init))

static GHashTable *
lease_environment (BroIsolationLease *lease)
{
  GHashTable *env = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert (env, g_strdup ("HOME"), g_strdup (BRO_HOME_LEASE (lease)->home));
  return env;
}

static char *
lease_profile_path (BroIsolationLease *lease)
{
  return g_strdup (BRO_HOME_LEASE (lease)->profile);
}

static void
lease_nothing (BroIsolationLease *lease)
{
}

/* Whether a settings.conf line sets app_Key. */
static gboolean
is_key_line (const char *line)
{
  const char *t = line + strspn (line, " \t");
  if (!g_str_has_prefix (t, "app_Key"))
    return FALSE;
  t += strlen ("app_Key");
  return t[strspn (t, " \t")] == '=';
}

/* Removes the app_Key line from settings.conf, whatever wrote it: the registration key shouldn't wait in the job's
 * folder for retention. Best effort: the file is 0600 and pruned with the job. */
static void
lease_release (BroIsolationLease *lease)
{
  BroHomeLease *self = BRO_HOME_LEASE (lease);
  g_autoptr (GBytes) bytes = NULL;
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  g_autoptr (GString) kept = g_string_new (NULL);
  gboolean changed = FALSE, first = TRUE;
  gsize n = 0;
  if (!bro_file_system_exists (self->fs, self->conf) || !(bytes = bro_file_system_read (self->fs, self->conf, NULL)))
    return;
  text = g_strndup (g_bytes_get_data (bytes, &n), g_bytes_get_size (bytes));
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      if (is_key_line (lines[i]))
        {
          changed = TRUE;
          continue;
        }
      if (!first)
        g_string_append_c (kept, '\n');
      g_string_append (kept, lines[i]);
      first = FALSE;
    }
  if (changed)
    {
      g_autoptr (GBytes) out = g_bytes_new (kept->str, kept->len);
      bro_file_system_write_atomically (self->fs, self->conf, out, 0600, NULL);
    }
}

static void
bro_home_lease_finalize (GObject *object)
{
  BroHomeLease *self = BRO_HOME_LEASE (object);
  g_clear_object (&self->fs);
  g_free (self->home);
  g_free (self->conf);
  g_free (self->profile);
  G_OBJECT_CLASS (bro_home_lease_parent_class)->finalize (object);
}

static void
bro_home_lease_class_init (BroHomeLeaseClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_home_lease_finalize;
}

static void
bro_home_lease_init (BroHomeLease *self)
{
}

static void
bro_home_lease_iface_init (BroIsolationLeaseInterface *iface)
{
  iface->environment = lease_environment;
  iface->profile_path = lease_profile_path;
  iface->first_output = lease_nothing;
  iface->release = lease_release;
}

/* ---- the isolation ---- */

struct _BroHomeDirIsolation {
  GObject parent_instance;
  BroFileSystem *fs;
  BroHomeLayout layout;
};

static void bro_home_dir_isolation_iface_init (BroSettingsIsolationInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroHomeDirIsolation, bro_home_dir_isolation, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_SETTINGS_ISOLATION, bro_home_dir_isolation_iface_init))

static gboolean
write_text (BroFileSystem *fs, const char *path, const char *text, int mode, BroBroError **error)
{
  g_autoptr (GBytes) bytes = g_bytes_new (text, strlen (text));
  return bro_file_system_write_atomically (fs, path, bytes, mode, error);
}

BroIsolationLease *
bro_home_dir_isolation_prepare (BroHomeDirIsolation *self, const BroMakemkvRunSettings *settings, BroBroError **error)
{
  const char *home = settings->work_directory;
  g_autofree char *folder = g_strconcat (home, self->layout == BRO_HOME_LAYOUT_MACOS ? "/Library/MakeMKV" : "/.MakeMKV", NULL);
  g_autofree char *conf_path = g_strconcat (folder, "/settings.conf", NULL);
  g_autofree char *conf = bro_settings_conf_render (settings->settings, "Written by Bromelia for one makemkvcon run");
  BroHomeLease *lease;
  if (!bro_file_system_create_directory (self->fs, folder, FALSE, error) || !write_text (self->fs, conf_path, conf, 0600, error))
    return NULL;
  lease = g_object_new (BRO_TYPE_HOME_LEASE, NULL);
  lease->fs = g_object_ref (self->fs);
  lease->home = g_strdup (home);
  lease->conf = g_strdup (conf_path);
  if (settings->profile_xml)
    {
      lease->profile = g_strconcat (home, "/profile.mmcp.xml", NULL);
      if (!write_text (self->fs, lease->profile, settings->profile_xml, 0644, error))
        {
          g_object_unref (lease);
          return NULL;
        }
    }
  return BRO_ISOLATION_LEASE (lease);
}

static BroIsolationLease *
prepare_vfunc (BroSettingsIsolation *self, const BroMakemkvRunSettings *settings, BroBroError **error)
{
  return bro_home_dir_isolation_prepare (BRO_HOME_DIR_ISOLATION (self), settings, error);
}

static void
bro_home_dir_isolation_finalize (GObject *object)
{
  g_clear_object (&BRO_HOME_DIR_ISOLATION (object)->fs);
  G_OBJECT_CLASS (bro_home_dir_isolation_parent_class)->finalize (object);
}

static void
bro_home_dir_isolation_class_init (BroHomeDirIsolationClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_home_dir_isolation_finalize;
}

static void
bro_home_dir_isolation_init (BroHomeDirIsolation *self)
{
}

static void
bro_home_dir_isolation_iface_init (BroSettingsIsolationInterface *iface)
{
  iface->prepare = prepare_vfunc;
}

BroHomeDirIsolation *
bro_home_dir_isolation_new (BroFileSystem *fs, BroHomeLayout layout)
{
  BroHomeDirIsolation *self = g_object_new (BRO_TYPE_HOME_DIR_ISOLATION, NULL);
  self->fs = g_object_ref (fs);
  self->layout = layout;
  return self;
}
