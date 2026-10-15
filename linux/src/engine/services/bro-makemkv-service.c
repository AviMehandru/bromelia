/* bro-makemkv-service.c */
#include "bro-makemkv-service.h"

#include "bro-profile-xml.h"
#include "bro-settings-conf.h"
#include "bro-settings-layers.h"
#include <string.h>

struct _BroMakemkvService {
  GObject parent_instance;
  BroFileSystem *fs;
  BroConfigService *config;
  char *data_dir;
  char *user_settings; /* nullable */
  char *home;
};

static void makemkv_setup_iface_init (BroMakemkvSetupInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroMakemkvService, bro_makemkv_service, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MAKEMKV_SETUP, makemkv_setup_iface_init))

static void
bro_makemkv_service_finalize (GObject *object)
{
  BroMakemkvService *self = BRO_MAKEMKV_SERVICE (object);
  g_clear_object (&self->fs);
  g_clear_object (&self->config);
  g_free (self->data_dir);
  g_free (self->user_settings);
  g_free (self->home);
  G_OBJECT_CLASS (bro_makemkv_service_parent_class)->finalize (object);
}

static void
bro_makemkv_service_class_init (BroMakemkvServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_makemkv_service_finalize;
}

static void
bro_makemkv_service_init (BroMakemkvService *self)
{
}

BroMakemkvService *
bro_makemkv_service_new (BroFileSystem *fs, BroConfigService *config, const char *data_dir, const char *user_settings, const char *home)
{
  BroMakemkvService *self = g_object_new (BRO_TYPE_MAKEMKV_SERVICE, NULL);
  self->fs = g_object_ref (fs);
  self->config = g_object_ref (config);
  self->data_dir = g_strdup (data_dir);
  self->user_settings = g_strdup (user_settings);
  self->home = g_strdup (home);
  return self;
}

/* ~ and ~/… under the user's home; anything else as it is. A new string. */
static char *
expand (BroMakemkvService *self, const char *path)
{
  if (strcmp (path, "~") == 0)
    return g_strdup (self->home);
  if (g_str_has_prefix (path, "~/"))
    return g_strconcat (self->home, path + 1, NULL);
  return g_strdup (path);
}

/* A JSON object of strings as a char * → char * table. */
static GHashTable *
strings (const BroJsonValue *map)
{
  GHashTable *o = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (map && map->kind == BRO_JSON_VALUE_OBJECT)
    for (guint i = 0; i < map->keys->len; i++)
      g_hash_table_insert (o, g_strdup (map->keys->pdata[i]), g_strdup (bro_json_value_get_string (map->items->pdata[i], "")));
  return o;
}

/* @obj (an object, or NULL) with @key set to @value. A new value. */
static BroJsonValue *
with (const BroJsonValue *obj, const char *key, const char *value)
{
  BroJsonValue *o = bro_json_value_new_object ();
  if (obj && obj->kind == BRO_JSON_VALUE_OBJECT)
    for (guint i = 0; i < obj->keys->len; i++)
      if (strcmp (obj->keys->pdata[i], key) != 0)
        bro_json_value_set (o, obj->keys->pdata[i], bro_json_value_ref (obj->items->pdata[i]));
  bro_json_value_set (o, key, bro_json_value_new_string (value));
  return o;
}

/* Hands @failure to the caller's @error, or frees it. */
static void
propagate (BroBroError **error, BroBroError *failure)
{
  if (error && !*error)
    *error = failure;
  else
    bro_bro_error_free (failure);
}

static gboolean
blank (const char *s)
{
  for (; *s; s++)
    if (!g_ascii_isspace (*s))
      return FALSE;
  return TRUE;
}

/* makemkv.registrationKey's secret when it has a value, else app_Key in the user's settings.conf, else none (NULL with
 * no error). */
static char *
registration_key (BroMakemkvService *self, const BroConfig *config, BroBroError **error)
{
  const char *name = bro_json_value_get_string (
      bro_json_value_member (bro_json_value_member (bro_json_value_member (config->document, "makemkv"), "registrationKey"), "secret"), NULL);
  if (name)
    {
      BroBroError *failure = NULL;
      char *key = bro_config_service_secret (self->config, name, config, &failure);
      if (failure)
        {
          propagate (error, failure);
          return NULL;
        }
      if (key && *key)
        return key;
      g_free (key);
    }
  if (!self->user_settings || !bro_file_system_exists (self->fs, self->user_settings))
    return NULL;
  g_autoptr (GBytes) bytes = bro_file_system_read (self->fs, self->user_settings, error);
  if (!bytes)
    return NULL;
  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);
  g_autofree char *text = g_strndup (data, size);
  g_autoptr (GHashTable) installed = bro_settings_conf_parse (text);
  const char *key = g_hash_table_lookup (installed, "app_Key");
  return key && *key ? g_strdup (key) : NULL;
}

static BroMakemkvInvocation *
invocation_vfunc (BroMakemkvSetup *setup, const BroConfig *config, const BroEffectiveProfile *profile, const char *selection_override,
                  const char *job_directory, BroBroError **error)
{
  BroMakemkvService *self = BRO_MAKEMKV_SERVICE (setup);
  BroJsonValue *makemkv = bro_json_value_member (profile->profile, "makemkv");
  BroJsonValue *file = bro_json_value_member (makemkv, "profile");
  const char *mode = bro_json_value_get_string (bro_json_value_member (file, "mode"), "makemkvDefault");
  BroJsonValue *generated = bro_json_value_member (file, "generated");
  const char *rule = bro_json_value_get_string (bro_json_value_member (generated, "selectionRule"), "");
  g_autofree char *xml = NULL;
  const char *profile_path = NULL;
  const char *selection = selection_override;
  BroBroError *failure = NULL;
  if (strcmp (mode, "generated") == 0)
    {
      g_autoptr (BroJsonValue) chosen = selection_override ? with (generated, "selectionRule", selection_override)
                                                           : (generated ? bro_json_value_ref (generated) : bro_json_value_new_object ());
      xml = bro_profile_xml_render (chosen);
      if (!selection && !blank (rule))
        selection = rule;
    }
  else if (strcmp (mode, "customFile") == 0)
    {
      g_autofree char *custom = expand (self, bro_json_value_get_string (bro_json_value_member (file, "customPath"), ""));
      if (*custom)
        profile_path = g_intern_string (custom); /* the options borrow it */
    }

  g_autofree char *key = registration_key (self, config, &failure);
  if (failure)
    {
      propagate (error, failure);
      return NULL;
    }
  g_autoptr (GHashTable) global = strings (bro_json_value_member (bro_json_value_member (config->document, "makemkv"), "settings"));
  g_autoptr (GHashTable) own = strings (bro_json_value_member (makemkv, "settings"));
  g_autoptr (GHashTable) drive = strings (NULL);
  BroMakemkvInvocation *inv = bro_makemkv_invocation_new ();
  g_hash_table_unref (inv->settings->settings);
  inv->settings->settings = bro_settings_layers_merge (global, own, drive, key, selection, self->data_dir);
  const char *data_dir = g_hash_table_lookup (inv->settings->settings, "app_DataDir");
  if (data_dir)
    g_hash_table_insert (inv->settings->settings, g_strdup ("app_DataDir"), expand (self, data_dir));
  inv->settings->profile_xml = g_steal_pointer (&xml);
  inv->settings->data_dir = g_strdup (self->data_dir);
  inv->settings->work_directory = g_strconcat (job_directory, "/home", NULL);

  BroJsonValue *v;
  inv->options = BRO_MAKEMKV_OPTIONS_NONE;
  inv->options.profile_path = profile_path;
  if ((v = bro_json_value_member (makemkv, "minLengthSeconds")) && v->kind == BRO_JSON_VALUE_INTEGER)
    inv->options.min_length_seconds = (int) v->integer;
  if ((v = bro_json_value_member (makemkv, "cacheMB")) && v->kind == BRO_JSON_VALUE_INTEGER)
    inv->options.cache_mb = (int) v->integer;
  if ((v = bro_json_value_member (makemkv, "directIO")) && v->kind == BRO_JSON_VALUE_BOOL)
    {
      inv->options.has_direct_io = TRUE;
      inv->options.direct_io = v->boolean;
    }
  inv->options.extra_arguments = g_intern_string (bro_json_value_get_string (bro_json_value_member (makemkv, "extraArguments"), ""));
  gint64 stall = bro_json_value_get_integer (bro_json_value_member (bro_json_value_member (config->document, "limits"), "stallTimeoutMinutes"), 0);
  if (stall > 0)
    {
      inv->has_stall_timeout = TRUE;
      inv->stall_timeout = (BroDuration) { (double) (stall * 60) };
    }
  inv->transcript = g_strconcat (job_directory, "/makemkv.txt", NULL);
  return inv;
}

static void
makemkv_setup_iface_init (BroMakemkvSetupInterface *iface)
{
  iface->invocation = invocation_vfunc;
}
