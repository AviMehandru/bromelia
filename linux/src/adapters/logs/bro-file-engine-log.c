/* bro-file-engine-log.c */
#include "bro-file-engine-log.h"

struct _BroFileEngineLog {
  GObject parent_instance;
  BroFileSystem *fs;
  BroClock *clock;
  char *path;
  GMutex lock;
};

static void engine_log_iface_init (BroEngineLogInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroFileEngineLog, bro_file_engine_log, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_ENGINE_LOG, engine_log_iface_init))

static void
bro_file_engine_log_finalize (GObject *object)
{
  BroFileEngineLog *self = BRO_FILE_ENGINE_LOG (object);
  g_clear_object (&self->fs);
  g_clear_object (&self->clock);
  g_free (self->path);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_file_engine_log_parent_class)->finalize (object);
}

static void
bro_file_engine_log_class_init (BroFileEngineLogClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_file_engine_log_finalize;
}

static void
bro_file_engine_log_init (BroFileEngineLog *self)
{
  g_mutex_init (&self->lock);
}

BroFileEngineLog *
bro_file_engine_log_new (BroFileSystem *fs, BroClock *clock, const char *path)
{
  BroFileEngineLog *self = g_object_new (BRO_TYPE_FILE_ENGINE_LOG, NULL);
  self->fs = g_object_ref (fs);
  self->clock = g_object_ref (clock);
  self->path = g_strdup (path);
  return self;
}

/* The canonical encoding on one line (as JobLog writes): its line breaks and their indentation are formatting only (a
 * string never holds a raw line break). */
static void
append_line (GByteArray *out, BroJsonValue *value)
{
  g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (value);
  gsize n;
  const guint8 *s = g_bytes_get_data (bytes, &n);
  for (gsize i = 0; i < n; i++)
    {
      if (s[i] != '\n')
        {
          g_byte_array_append (out, &s[i], 1);
          continue;
        }
      while (i + 1 < n && s[i + 1] == ' ')
        i++;
    }
  g_byte_array_append (out, (const guint8 *) "\n", 1);
}

void
bro_file_engine_log_log (BroFileEngineLog *self, const BroBroMessage *message)
{
  g_autoptr (BroJsonValue) o = bro_json_value_new_object ();
  g_autofree char *t = bro_instant_format (bro_clock_now (self->clock));
  g_autofree char *folder = g_path_get_dirname (self->path);
  g_autoptr (GByteArray) all = g_byte_array_new ();
  g_autoptr (GBytes) before = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (BroBroError) error = NULL;
  bro_json_value_set (o, "t", bro_json_value_new_string (t));
  bro_json_value_set (o, "severity", bro_json_value_new_string (bro_severity_to_wire (message->severity)));
  bro_json_value_set (o, "code", bro_json_value_new_string (bro_message_code_wire (message->code)));
  if (bro_json_value_length (message->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (message->params));
  g_mutex_lock (&self->lock);
  if (!bro_file_system_exists (self->fs, folder) && !bro_file_system_create_directory (self->fs, folder, FALSE, &error))
    goto out;
  if (bro_file_system_exists (self->fs, self->path) && !(before = bro_file_system_read (self->fs, self->path, &error)))
    goto out;
  if (before)
    g_byte_array_append (all, g_bytes_get_data (before, NULL), g_bytes_get_size (before));
  append_line (all, o);
  bytes = g_bytes_new (all->data, all->len);
  bro_file_system_write_atomically (self->fs, self->path, bytes, 0600, &error);
out:
  g_mutex_unlock (&self->lock);
}

static void
log_line (BroEngineLog *self, const BroBroMessage *message)
{
  bro_file_engine_log_log (BRO_FILE_ENGINE_LOG (self), message);
}

static void
engine_log_iface_init (BroEngineLogInterface *iface)
{
  iface->log = log_line;
}
