/* bro-platform-keystore.c */
#include "bro-platform-keystore.h"

#include "bro-message-code.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif
#include <unistd.h>
#ifdef BRO_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

struct _BroPlatformKeystore {
  GObject parent_instance;
  char *fallback_dir;
  char *service;
  GMutex lock;
  int system; /* -1 not asked yet, 0 the files, 1 the Secret Service */
#ifdef BRO_HAVE_LIBSECRET
  SecretService *secrets;
#endif
};

static void bro_platform_keystore_iface_init (BroKeystoreInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlatformKeystore, bro_platform_keystore, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_KEYSTORE, bro_platform_keystore_iface_init))

static gboolean
failed (BroBroError **error, const char *name, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "name", bro_json_value_new_string (name ? name : ""));
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_KEYSTORE_FAILED), params);
  return FALSE;
}

/* A SecretRef name: ^[a-z][a-zA-Z0-9]*(\.[A-Za-z0-9_-]+)*$. */
static gboolean
is_secret_name (const char *name)
{
  const char *p = name;
  if (!name || *p < 'a' || *p > 'z')
    return FALSE;
  for (p++; g_ascii_isalnum (*p); p++)
    ;
  while (*p)
    {
      const char *start;
      if (*p != '.')
        return FALSE;
      start = ++p;
      while (g_ascii_isalnum (*p) || *p == '_' || *p == '-')
        p++;
      if (p == start)
        return FALSE;
    }
  return TRUE;
}

static gboolean
check (const char *name, BroBroError **error)
{
  return is_secret_name (name) ? TRUE : failed (error, name, "not a secret name");
}

/* ---- the Secret Service ---- */

#ifdef BRO_HAVE_LIBSECRET
static const SecretSchema *
schema (void)
{
  static const SecretSchema s = { "app.bromelia.Bromelia.Secret",
                                  SECRET_SCHEMA_NONE,
                                  { { "service", SECRET_SCHEMA_ATTRIBUTE_STRING }, { "name", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } } };
  return &s;
}
#endif

/* The Secret Service, unless none answers (asked once: opening a session starts the service and never prompts). */
static gboolean
use_system (BroPlatformKeystore *self)
{
  gboolean usable;
  g_mutex_lock (&self->lock);
  if (self->system < 0)
    {
#ifdef BRO_HAVE_LIBSECRET
      g_autoptr (GError) gerror = NULL;
      self->secrets = secret_service_get_sync (SECRET_SERVICE_NONE, NULL, &gerror);
      if (self->secrets && !secret_service_ensure_session_sync (self->secrets, NULL, &gerror))
        g_clear_object (&self->secrets);
      self->system = self->secrets != NULL;
      if (gerror)
        g_debug ("keystore: no Secret Service (%s); secrets are files in %s", gerror->message, self->fallback_dir);
#else
      self->system = 0;
#endif
    }
  usable = self->system == 1;
  g_mutex_unlock (&self->lock);
  return usable;
}

#ifdef BRO_HAVE_LIBSECRET
/* The items of @name, never unlocking anything (unlocking would prompt, and nobody may be there to answer). */
static GList *
find (BroPlatformKeystore *self, const char *name, SecretSearchFlags flags, GError **gerror)
{
  g_autoptr (GHashTable) attributes = secret_attributes_build (schema (), "service", self->service, "name", name, NULL);
  return secret_service_search_sync (self->secrets, schema (), attributes, flags, NULL, gerror);
}

static gboolean
locked (GList *items)
{
  for (GList *l = items; l; l = l->next)
    if (secret_item_get_locked (l->data))
      return TRUE;
  return FALSE;
}
#endif

/* ---- the files ---- */

static gboolean
not_private (BroPlatformKeystore *self, BroBroError **error, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "path", bro_json_value_new_string (self->fallback_dir));
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_NOT_PRIVATE), params);
  return FALSE;
}

/* Whether @path (or the nearest folder above it that exists) is on a network file system: NFS, SMB / CIFS, AFS, Coda,
 * NCP or Ceph. FUSE file systems (sshfs) aren't recognised. (A build on macOS, for development, asks MNT_LOCAL.) */
static gboolean
on_network_share (const char *path)
{
  static const unsigned long network[] = { 0x6969 /* NFS */,     0x517B /* SMB */,      0xFF534D42 /* CIFS */,
                                           0xFE534D42 /* SMB2 */, 0x5346414F /* AFS */,  0x73757245 /* Coda */,
                                           0x564C /* NCP */,      0x00C36400 /* Ceph */ };
  g_autofree char *at = g_strdup (path);
  struct statfs fs;
  while (statfs (at, &fs) != 0)
    {
      char *up = g_path_get_dirname (at);
      if (strcmp (up, at) == 0)
        {
          g_free (up);
          return FALSE;
        }
      g_free (at);
      at = up;
    }
#ifdef __APPLE__
  (void) network;
  return (fs.f_flags & MNT_LOCAL) == 0;
#else
  for (gsize i = 0; i < G_N_ELEMENTS (network); i++)
    if ((unsigned long) fs.f_type == network[i])
      return TRUE;
  return FALSE;
#endif
}

/* fs.notPrivate when the fallback folder is on a network share: the server decides who reads the files there,
 * whatever mode is asked for. */
static gboolean
refuse_network (BroPlatformKeystore *self, BroBroError **error)
{
  return on_network_share (self->fallback_dir) ? not_private (self, error, "it is on a network share") : TRUE;
}

static char *
file_path (BroPlatformKeystore *self, const char *name)
{
  return g_build_filename (self->fallback_dir, name, NULL);
}

static char *
read_file (BroPlatformKeystore *self, const char *name, BroBroError **error)
{
  g_autofree char *path = file_path (self, name);
  g_autoptr (GError) gerror = NULL;
  char *text = NULL;
  if (!refuse_network (self, error))
    return NULL;
  if (g_file_get_contents (path, &text, NULL, &gerror))
    return text;
  if (!g_error_matches (gerror, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    failed (error, name, gerror->message);
  return NULL;
}

/* Written next to the final file (0600 from the start), flushed, then renamed over it. */
static gboolean
write_file (BroPlatformKeystore *self, const char *name, const char *value, BroBroError **error)
{
  g_autofree char *path = file_path (self, name);
  g_autofree char *uuid = g_uuid_string_random ();
  g_autofree char *base = g_strdup_printf (".%s.%s.tmp", name, uuid);
  g_autofree char *temp = g_build_filename (self->fallback_dir, base, NULL);
  size_t length = strlen (value), written = 0;
  int fd, saved;
  struct stat st;
  if (!refuse_network (self, error))
    return FALSE;
  if (g_mkdir_with_parents (self->fallback_dir, 0700) != 0)
    return failed (error, name, g_strerror (errno));
  if (chmod (self->fallback_dir, 0700) != 0 || lstat (self->fallback_dir, &st) != 0 || !S_ISDIR (st.st_mode)
      || st.st_uid != geteuid () || (st.st_mode & 077) != 0)
    return not_private (self, error, "the file system didn't keep its owner-only permissions");
  fd = open (temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    return failed (error, name, g_strerror (errno));
  while (written < length)
    {
      ssize_t n = write (fd, value + written, length - written);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      written += (size_t) n;
    }
  saved = written == length && fsync (fd) == 0 ? 0 : (errno ? errno : EIO);
  close (fd);
  if (saved == 0 && rename (temp, path) != 0)
    saved = errno;
  if (saved != 0)
    {
      g_unlink (temp);
      return failed (error, name, g_strerror (saved));
    }
  return TRUE;
}

/* ---- the port ---- */

char *
bro_platform_keystore_get (BroPlatformKeystore *self, const char *name, BroBroError **error)
{
  if (!check (name, error))
    return NULL;
  if (!use_system (self))
    return read_file (self, name, error);
#ifdef BRO_HAVE_LIBSECRET
  {
    g_autoptr (GError) gerror = NULL;
    GList *items = find (self, name, SECRET_SEARCH_LOAD_SECRETS, &gerror);
    g_autoptr (SecretValue) value = NULL;
    const char *bytes;
    gsize length;
    char *text = NULL;
    if (gerror)
      failed (error, name, gerror->message);
    else if (locked (items))
      failed (error, name, "the keyring is locked");
    else if (items && (value = secret_item_get_secret (items->data)) != NULL)
      {
        bytes = secret_value_get (value, &length);
        text = g_strndup (bytes, length);
      }
    else if (items)
      failed (error, name, "the Secret Service gave no value");
    g_list_free_full (items, g_object_unref);
    return text;
  }
#else
  return NULL;
#endif
}

gboolean
bro_platform_keystore_set (BroPlatformKeystore *self, const char *name, const char *value, BroBroError **error)
{
  if (!check (name, error))
    return FALSE;
  if (!use_system (self))
    return write_file (self, name, value, error);
#ifdef BRO_HAVE_LIBSECRET
  {
    /* The default keyring must exist and be unlocked: creating or unlocking one would prompt. */
    g_autoptr (GError) gerror = NULL;
    g_autoptr (SecretCollection) collection = secret_collection_for_alias_sync (self->secrets, SECRET_COLLECTION_DEFAULT,
                                                                                SECRET_COLLECTION_NONE, NULL, &gerror);
    g_autoptr (GHashTable) attributes = NULL;
    g_autoptr (SecretValue) secret = NULL;
    g_autofree char *label = NULL;
    if (!collection)
      return failed (error, name, gerror ? gerror->message : "there is no default keyring");
    if (secret_collection_get_locked (collection))
      return failed (error, name, "the keyring is locked");
    attributes = secret_attributes_build (schema (), "service", self->service, "name", name, NULL);
    secret = secret_value_new (value, -1, "text/plain");
    label = g_strdup_printf ("%s: %s", self->service, name);
    if (!secret_service_store_sync (self->secrets, schema (), attributes, SECRET_COLLECTION_DEFAULT, label, secret, NULL, &gerror))
      return failed (error, name, gerror ? gerror->message : "the Secret Service refused");
    return TRUE;
  }
#else
  return FALSE;
#endif
}

gboolean
bro_platform_keystore_remove (BroPlatformKeystore *self, const char *name, BroBroError **error)
{
  if (!check (name, error))
    return FALSE;
  if (!use_system (self))
    {
      g_autofree char *path = file_path (self, name);
      if (g_unlink (path) != 0 && errno != ENOENT)
        return failed (error, name, g_strerror (errno));
      return TRUE;
    }
#ifdef BRO_HAVE_LIBSECRET
  {
    g_autoptr (GError) search_error = NULL;
    GList *items = find (self, name, SECRET_SEARCH_ALL, &search_error);
    gboolean ok = search_error ? failed (error, name, search_error->message) : TRUE;
    if (ok && locked (items))
      ok = failed (error, name, "the keyring is locked");
    for (GList *l = items; ok && l; l = l->next)
      {
        g_autoptr (GError) gerror = NULL;
        if (!secret_item_delete_sync (l->data, NULL, &gerror))
          ok = failed (error, name, gerror->message);
      }
    g_list_free_full (items, g_object_unref);
    return ok;
  }
#else
  return FALSE;
#endif
}

const char *
bro_platform_keystore_backend (BroPlatformKeystore *self)
{
  return use_system (self) ? "secretService" : "file";
}

static char *get_vfunc (BroKeystore *self, const char *name, BroBroError **error) { return bro_platform_keystore_get (BRO_PLATFORM_KEYSTORE (self), name, error); }
static gboolean set_vfunc (BroKeystore *self, const char *name, const char *value, BroBroError **error) { return bro_platform_keystore_set (BRO_PLATFORM_KEYSTORE (self), name, value, error); }
static gboolean remove_vfunc (BroKeystore *self, const char *name, BroBroError **error) { return bro_platform_keystore_remove (BRO_PLATFORM_KEYSTORE (self), name, error); }

static void
bro_platform_keystore_iface_init (BroKeystoreInterface *iface)
{
  iface->get = get_vfunc;
  iface->set = set_vfunc;
  iface->remove = remove_vfunc;
}

static void
bro_platform_keystore_finalize (GObject *object)
{
  BroPlatformKeystore *self = BRO_PLATFORM_KEYSTORE (object);
  g_free (self->fallback_dir);
  g_free (self->service);
#ifdef BRO_HAVE_LIBSECRET
  g_clear_object (&self->secrets);
#endif
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_platform_keystore_parent_class)->finalize (object);
}

static void bro_platform_keystore_class_init (BroPlatformKeystoreClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_platform_keystore_finalize; }

static void
bro_platform_keystore_init (BroPlatformKeystore *self)
{
  g_mutex_init (&self->lock);
  self->system = -1;
}

BroPlatformKeystore *
bro_platform_keystore_new (const char *fallback_dir, const char *service, gboolean system_store)
{
  BroPlatformKeystore *self = g_object_new (BRO_TYPE_PLATFORM_KEYSTORE, NULL);
  self->fallback_dir = g_strdup (fallback_dir);
  self->service = g_strdup (service ? service : "Bromelia");
  if (!system_store)
    self->system = 0;
  return self;
}
