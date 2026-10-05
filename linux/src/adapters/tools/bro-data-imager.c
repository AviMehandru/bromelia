/* bro-data-imager.c */
#include "bro-data-imager.h"

#include "bro-message-code.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define SECTOR 2048
#define CHUNK 512
#define PROGRESS_MAX 10000

struct _BroDataImager {
  GObject parent_instance;
  BroDriveControl *drives;
  BroFileSystem *fs;
};

G_DEFINE_FINAL_TYPE (BroDataImager, bro_data_imager, G_TYPE_OBJECT)

static void
read_error (BroBroError **error, gint64 offset, const char *reason)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "offset", bro_json_value_new_integer (offset));
  bro_json_value_set (params, "reason", bro_json_value_new_string (reason));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_OTHER_READ_ERROR), params);
}

static void
write_failed (BroBroError **error, const char *path, int code)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "operation", bro_json_value_new_string ("write"));
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_json_value_set (params, "reason", bro_json_value_new_string (g_strerror (code)));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_FAILED), params);
}

/* A chunk; when it can't be read whole, its sectors one by one, so the error names the first bad byte (a short read is
 * an error too: the image is never shorter than the disc). */
static GBytes *
read_chunk (BroSectorReader *reader, gint64 sector, int count, BroBroError **error)
{
  g_autoptr (BroBroError) whole_error = NULL;
  GBytes *whole = bro_sector_reader_read (reader, sector, count, &whole_error);
  GByteArray *data;
  if (whole && g_bytes_get_size (whole) == (gsize) count * SECTOR)
    return whole;
  g_clear_pointer (&whole, g_bytes_unref);
  data = g_byte_array_sized_new ((guint) count * SECTOR);
  for (int i = 0; i < count; i++)
    {
      g_autoptr (BroBroError) one_error = NULL;
      g_autoptr (GBytes) one = bro_sector_reader_read (reader, sector + i, 1, &one_error);
      gint64 at = (sector + i) * SECTOR;
      if (!one)
        {
          const char *reason = one_error ? bro_json_value_get_string (bro_json_value_member (one_error->params, "reason"), one_error->code) : "unreadable";
          read_error (error, at, reason);
          g_byte_array_unref (data);
          return NULL;
        }
      if (g_bytes_get_size (one) != SECTOR)
        {
          read_error (error, at, "short read");
          g_byte_array_unref (data);
          return NULL;
        }
      g_byte_array_append (data, g_bytes_get_data (one, NULL), SECTOR);
    }
  return g_byte_array_free_to_bytes (data);
}

gint64
bro_data_imager_copy (BroDataImager *self, const char *device, const char *dest_iso, BroRunSink *sink, BroCancellationToken *cancel,
                      BroBroError **error)
{
  g_autoptr (BroSectorReader) reader = NULL;
  g_autofree char *folder = g_path_get_dirname (dest_iso), *name = g_path_get_basename (dest_iso), *uuid = g_uuid_string_random ();
  g_autofree char *part_name = NULL, *part = NULL;
  gint64 total, copied = 0, result = -1;
  int fd = -1;
  if (bro_file_system_exists (self->fs, dest_iso))
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (dest_iso));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_ALREADY_EXISTS), params);
      return -1;
    }
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return -1;
    }
  reader = bro_drive_control_open_raw (self->drives, device, error);
  if (!reader)
    return -1;
  uuid[8] = '\0';
  part_name = g_strdup_printf (".%s.part-%s", name, uuid);
  part = g_build_filename (folder, part_name, NULL);
  total = bro_sector_reader_sector_count (reader, error);
  if (total < 0)
    goto out;
  if (total == 0)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_OTHER_EMPTY_DISC), NULL);
      goto out;
    }
  fd = g_open (part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
    {
      write_failed (error, part, errno);
      goto out;
    }
  while (copied < total)
    {
      int count = (int) MIN ((gint64) CHUNK, total - copied);
      g_autoptr (GBytes) data = NULL;
      const guint8 *bytes;
      gsize size, written = 0;
      int share;
      if (cancel && bro_cancellation_token_is_cancelled (cancel))
        {
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
          goto out;
        }
      data = read_chunk (reader, copied, count, error);
      if (!data)
        goto out;
      bytes = g_bytes_get_data (data, &size);
      while (written < size)
        {
          ssize_t n = write (fd, bytes + written, size - written);
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            {
              write_failed (error, part, errno ? errno : EIO);
              goto out;
            }
          written += (gsize) n;
        }
      copied += count;
      share = (int) (copied * PROGRESS_MAX / total);
      {
        g_autoptr (BroRobotEvent) event = bro_robot_event_new (BRO_ROBOT_EVENT_PROGRESS_VALUE);
        event->current = share;
        event->total = share;
        event->max = PROGRESS_MAX;
        bro_run_sink_event (sink, event);
      }
    }
  close (fd);
  fd = -1;
  if (!bro_file_system_sync_file (self->fs, part, error))
    goto out;
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      goto out;
    }
  if (!bro_file_system_rename (self->fs, part, dest_iso, error) || !bro_file_system_sync_directory (self->fs, folder, error))
    goto out;
  result = copied * SECTOR;
out:
  if (fd >= 0)
    close (fd);
  bro_sector_reader_close (reader);
  if (result < 0)
    g_unlink (part);
  return result;
}

static void
bro_data_imager_finalize (GObject *object)
{
  BroDataImager *self = BRO_DATA_IMAGER (object);
  g_clear_object (&self->drives);
  g_clear_object (&self->fs);
  G_OBJECT_CLASS (bro_data_imager_parent_class)->finalize (object);
}

static void bro_data_imager_class_init (BroDataImagerClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_data_imager_finalize; }
static void bro_data_imager_init (BroDataImager *self) {}

BroDataImager *
bro_data_imager_new (BroDriveControl *drives, BroFileSystem *fs)
{
  BroDataImager *self = g_object_new (BRO_TYPE_DATA_IMAGER, NULL);
  self->drives = g_object_ref (drives);
  self->fs = g_object_ref (fs);
  return self;
}
