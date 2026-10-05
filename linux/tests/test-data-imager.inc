/* test-data-imager.inc: BroDataImager, included by test-adapters.c: shared/fixtures/adapters/data-imager.cases.json
 * against a disc made up on the fly. */

static guint8
fake_disc_at (gint64 i)
{
  return (guint8) ((i / 2048 + i) % 256);
}

/* Byte i is (i / 2048 + i) mod 256; some sectors can't be read. */
#define TEST_TYPE_FAKE_DISC (test_fake_disc_get_type ())
G_DECLARE_FINAL_TYPE (TestFakeDisc, test_fake_disc, TEST, FAKE_DISC, GObject)
struct _TestFakeDisc {
  GObject parent_instance;
  gint64 sectors;
  GHashTable *unreadable; /* gint64 sector + 1 as a pointer */
  int cancel_after;       /* reads; 0: never */
  BroCancellationToken *cancel;
  int reads;
  gboolean closed;
  gboolean size_fails;
};
static void test_fake_disc_iface_init (BroSectorReaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestFakeDisc, test_fake_disc, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_SECTOR_READER, test_fake_disc_iface_init))

static GBytes *
fd_read (BroSectorReader *r, gint64 sector, int count, BroBroError **error)
{
  TestFakeDisc *self = TEST_FAKE_DISC (r);
  gint64 n;
  guint8 *data;
  for (gint64 s = sector; s < sector + count; s++)
    if (g_hash_table_contains (self->unreadable, GSIZE_TO_POINTER ((gsize) s + 1)))
      {
        BroJsonValue *params = bro_json_value_new_object ();
        bro_json_value_set (params, "operation", bro_json_value_new_string ("read"));
        bro_json_value_set (params, "path", bro_json_value_new_string ("/dev/sr0"));
        bro_json_value_set (params, "reason", bro_json_value_new_string ("I/O error"));
        bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_FAILED), params);
        return NULL;
      }
  n = MAX (0, MIN ((gint64) count, self->sectors - sector)) * 2048;
  data = g_malloc (n ? n : 1);
  for (gint64 i = 0; i < n; i++)
    data[i] = fake_disc_at (sector * 2048 + i);
  self->reads++;
  if (self->cancel_after && self->reads == self->cancel_after)
    bro_cancellation_token_cancel (self->cancel);
  return g_bytes_new_take (data, (gsize) n);
}

static gint64
fd_count (BroSectorReader *r, BroBroError **error)
{
  if (TEST_FAKE_DISC (r)->size_fails)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string ("/dev/sr0"));
      bro_json_value_set (params, "reason", bro_json_value_new_string ("Inappropriate ioctl for device"));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_DRIVE_SIZE_UNKNOWN), params);
      return -1;
    }
  return TEST_FAKE_DISC (r)->sectors;
}
static void fd_close (BroSectorReader *r) { TEST_FAKE_DISC (r)->closed = TRUE; }
static void test_fake_disc_finalize (GObject *o) { g_hash_table_unref (TEST_FAKE_DISC (o)->unreadable); G_OBJECT_CLASS (test_fake_disc_parent_class)->finalize (o); }
static void test_fake_disc_class_init (TestFakeDiscClass *k) { G_OBJECT_CLASS (k)->finalize = test_fake_disc_finalize; }
static void test_fake_disc_init (TestFakeDisc *self) { self->unreadable = g_hash_table_new (g_direct_hash, g_direct_equal); }

static void
test_fake_disc_iface_init (BroSectorReaderInterface *iface)
{
  iface->read = fd_read;
  iface->sector_count = fd_count;
  iface->close = fd_close;
}

/* Opens the fake disc (or fails as a missing drive); the rest isn't used. */
#define TEST_TYPE_FAKE_DRIVE_CONTROL (test_fake_drive_control_get_type ())
G_DECLARE_FINAL_TYPE (TestFakeDriveControl, test_fake_drive_control, TEST, FAKE_DRIVE_CONTROL, GObject)
struct _TestFakeDriveControl {
  GObject parent_instance;
  TestFakeDisc *disc; /* nullable */
  gboolean opened;
};
static void test_fake_drive_control_iface_init (BroDriveControlInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestFakeDriveControl, test_fake_drive_control, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_CONTROL, test_fake_drive_control_iface_init))

static BroSectorReader *
fdc_open_raw (BroDriveControl *c, const char *device, BroBroError **error)
{
  TestFakeDriveControl *self = TEST_FAKE_DRIVE_CONTROL (c);
  self->opened = TRUE;
  if (!self->disc)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_DRIVE_NOT_FOUND), NULL);
      return NULL;
    }
  return g_object_ref (BRO_SECTOR_READER (self->disc));
}

static void test_fake_drive_control_finalize (GObject *o) { g_clear_object (&TEST_FAKE_DRIVE_CONTROL (o)->disc); G_OBJECT_CLASS (test_fake_drive_control_parent_class)->finalize (o); }
static void test_fake_drive_control_class_init (TestFakeDriveControlClass *k) { G_OBJECT_CLASS (k)->finalize = test_fake_drive_control_finalize; }
static void test_fake_drive_control_init (TestFakeDriveControl *self) {}
static void test_fake_drive_control_iface_init (BroDriveControlInterface *iface) { iface->open_raw = fdc_open_raw; }

/* The real file system, except that folder syncs fail, as on a share that refuses them. */
#define TEST_TYPE_SYNC_FAILING_FS (test_sync_failing_fs_get_type ())
G_DECLARE_FINAL_TYPE (TestSyncFailingFs, test_sync_failing_fs, TEST, SYNC_FAILING_FS, GObject)
struct _TestSyncFailingFs {
  GObject parent_instance;
  BroFileSystem *real;
};
static void test_sync_failing_fs_iface_init (BroFileSystemInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestSyncFailingFs, test_sync_failing_fs, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (BRO_TYPE_FILE_SYSTEM, test_sync_failing_fs_iface_init))
#define REAL(s) (TEST_SYNC_FAILING_FS (s)->real)
static gboolean sf_exists (BroFileSystem *s, const char *p) { return bro_file_system_exists (REAL (s), p); }
static BroFileInfo *sf_stat (BroFileSystem *s, const char *p, BroBroError **e) { return bro_file_system_stat (REAL (s), p, e); }
static GPtrArray *sf_list (BroFileSystem *s, const char *p, BroBroError **e) { return bro_file_system_list (REAL (s), p, e); }
static GBytes *sf_read (BroFileSystem *s, const char *p, BroBroError **e) { return bro_file_system_read (REAL (s), p, e); }
static GBytes *sf_read_range (BroFileSystem *s, const char *p, gint64 o, int n, BroBroError **e) { return bro_file_system_read_range (REAL (s), p, o, n, e); }
static gboolean sf_create_directory (BroFileSystem *s, const char *p, gboolean m, BroBroError **e) { return bro_file_system_create_directory (REAL (s), p, m, e); }
static gboolean sf_write_atomically (BroFileSystem *s, const char *p, GBytes *b, int m, BroBroError **e) { return bro_file_system_write_atomically (REAL (s), p, b, m, e); }
static gboolean sf_rename (BroFileSystem *s, const char *f, const char *t, BroBroError **e) { return bro_file_system_rename (REAL (s), f, t, e); }
static GPtrArray *
sf_move_merging (BroFileSystem *s, const char *f, const char *t, BroMovePolicy p, BroMovedFunc on_moved, gpointer data, BroBroError **e)
{
  return bro_file_system_move_merging (REAL (s), f, t, p, on_moved, data, e);
}
static gboolean sf_remove (BroFileSystem *s, const char *p, BroBroError **e) { return bro_file_system_remove (REAL (s), p, e); }
static gboolean sf_move_to_trash (BroFileSystem *s, const char *p, const char *t, BroBroError **e) { return bro_file_system_move_to_trash (REAL (s), p, t, e); }
static gboolean sf_sync_file (BroFileSystem *s, const char *p, BroBroError **e) { return bro_file_system_sync_file (REAL (s), p, e); }
static BroByteStream *sf_open (BroFileSystem *s, const char *p, gboolean b, BroBroError **e) { return bro_file_system_open_for_reading (REAL (s), p, b, e); }
static BroVolumeInfo *sf_volume (BroFileSystem *s, const char *p, BroBroError **e) { return bro_file_system_volume (REAL (s), p, e); }
#undef REAL

static gboolean
sf_sync_directory (BroFileSystem *s, const char *path, BroBroError **error)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "operation", bro_json_value_new_string ("sync"));
  bro_json_value_set (params, "path", bro_json_value_new_string (path));
  bro_json_value_set (params, "reason", bro_json_value_new_string ("Invalid argument"));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_FS_FAILED), params);
  return FALSE;
}

static void test_sync_failing_fs_finalize (GObject *o) { g_clear_object (&TEST_SYNC_FAILING_FS (o)->real); G_OBJECT_CLASS (test_sync_failing_fs_parent_class)->finalize (o); }
static void test_sync_failing_fs_class_init (TestSyncFailingFsClass *k) { G_OBJECT_CLASS (k)->finalize = test_sync_failing_fs_finalize; }
static void test_sync_failing_fs_init (TestSyncFailingFs *self) { self->real = BRO_FILE_SYSTEM (bro_platform_file_system_new ()); }

static void
test_sync_failing_fs_iface_init (BroFileSystemInterface *iface)
{
  iface->exists = sf_exists;
  iface->stat = sf_stat;
  iface->list = sf_list;
  iface->read = sf_read;
  iface->read_range = sf_read_range;
  iface->create_directory = sf_create_directory;
  iface->write_atomically = sf_write_atomically;
  iface->rename = sf_rename;
  iface->move_merging = sf_move_merging;
  iface->remove = sf_remove;
  iface->move_to_trash = sf_move_to_trash;
  iface->sync_file = sf_sync_file;
  iface->sync_directory = sf_sync_directory;
  iface->open_for_reading = sf_open;
  iface->volume = sf_volume;
}

static gboolean
data_imager_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestFakeDriveControl) drives = g_object_new (TEST_TYPE_FAKE_DRIVE_CONTROL, NULL);
  g_autoptr (TestFakeDisc) disc = g_object_new (TEST_TYPE_FAKE_DISC, NULL);
  g_autoptr (BroPlatformFileSystem) fs = bro_platform_file_system_new ();
  g_autoptr (TestSyncFailingFs) sync_failing = g_object_new (TEST_TYPE_SYNC_FAILING_FS, NULL);
  g_autoptr (TestRecordingSink) sink = g_object_new (TEST_TYPE_RECORDING_SINK, NULL);
  g_autoptr (BroCancellationToken) cancel = bro_cancellation_token_new ();
  g_autoptr (BroDataImager) imager = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *dir = g_build_filename (fs_root, "image", NULL), *dest = NULL;
  BroJsonValue *want_error = bro_json_value_member (expect, "error"), *v;
  g_autoptr (BroDataImageCopy) copy = NULL;
  remove_tree (fs_root);
  g_mkdir_with_parents (dir, 0755);
  dest = g_build_filename (dir, "disc.iso", NULL);
  if (bro_json_value_get_bool (bro_json_value_member (given, "existing"), FALSE))
    g_file_set_contents (dest, "already here", -1, NULL);
  disc->sectors = bro_json_value_get_integer (bro_json_value_member (given, "sectors"), 0);
  disc->size_fails = bro_json_value_get_bool (bro_json_value_member (given, "sizeFails"), FALSE);
  for (guint i = 0; i < bro_json_value_length (bro_json_value_member (given, "unreadable")); i++)
    g_hash_table_add (disc->unreadable,
                      GSIZE_TO_POINTER ((gsize) bro_json_value_get_integer (bro_json_value_at (bro_json_value_member (given, "unreadable"), i), 0) + 1));
  disc->cancel_after = (int) bro_json_value_get_integer (bro_json_value_member (given, "cancelAfterChunks"), 0);
  disc->cancel = cancel;
  if (!bro_json_value_get_bool (bro_json_value_member (given, "openFails"), FALSE))
    drives->disc = g_object_ref (disc);
  if (bro_json_value_get_bool (bro_json_value_member (given, "syncFails"), FALSE))
    imager = bro_data_imager_new (BRO_DRIVE_CONTROL (drives), BRO_FILE_SYSTEM (sync_failing));
  else
    imager = bro_data_imager_new (BRO_DRIVE_CONTROL (drives), BRO_FILE_SYSTEM (fs));
  copy = bro_data_imager_copy (imager, "/dev/sr0", dest, BRO_RUN_SINK (sink), cancel, &error);
  if (copy)
    {
      if (want_error)
        bro_test_fail (failures, id, "expected an error");
      if (copy->bytes != bro_json_value_get_integer (bro_json_value_member (expect, "bytes"), -1))
        bro_test_fail (failures, id, "bytes %" G_GINT64_FORMAT, copy->bytes);
      bro_test_same_string (failures, id, "warning", bro_json_value_get_string (bro_json_value_member (expect, "warning"), NULL),
                            copy->warning ? bro_message_code_wire (copy->warning->code) : NULL);
      if (copy->warning)
        bro_test_same_string (failures, id, "warning path", dest, bro_json_value_get_string (bro_json_value_member (copy->warning->params, "path"), NULL));
    }
  else if (!error)
    bro_test_fail (failures, id, "NULL without an error");
  if (error)
    {
      bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (want_error, "code"), NULL), error->code);
      for (guint i = 0; want_error && i < bro_json_value_length (bro_json_value_member (want_error, "params")); i++)
        {
          BroJsonValue *params = bro_json_value_member (want_error, "params");
          bro_test_same_json (failures, id, params->keys->pdata[i], params->items->pdata[i], bro_json_value_member (error->params, params->keys->pdata[i]));
        }
    }
  if ((v = bro_json_value_member (expect, "progress")))
    {
      g_autoptr (GPtrArray) want = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *want_text = NULL, *got_text = hb_joined (sink->progress);
      for (guint i = 0; i < bro_json_value_length (v); i++)
        g_ptr_array_add (want, g_strdup_printf ("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "/10000",
                                                bro_json_value_get_integer (bro_json_value_at (bro_json_value_at (v, i), 0), -1),
                                                bro_json_value_get_integer (bro_json_value_at (bro_json_value_at (v, i), 1), -1)));
      want_text = hb_joined (want);
      bro_test_same_string (failures, id, "progress", want_text, got_text);
    }
  if (bro_json_value_get_bool (bro_json_value_member (expect, "imageMatches"), FALSE))
    {
      g_autofree char *image = NULL;
      gsize n = 0;
      if (!g_file_get_contents (dest, &image, &n, NULL) || (gint64) n != disc->sectors * 2048)
        bro_test_fail (failures, id, "image size");
      else
        for (gsize i = 0; i < n; i++)
          if ((guint8) image[i] != fake_disc_at ((gint64) i))
            {
              bro_test_fail (failures, id, "byte %" G_GSIZE_FORMAT " differs", i);
              break;
            }
    }
  if ((v = bro_json_value_member (expect, "reads")) && (disc->reads != bro_json_value_get_integer (v, -1) || drives->opened))
    bro_test_fail (failures, id, "read or opened");
  if (bro_json_value_get_bool (bro_json_value_member (expect, "closed"), FALSE) && !disc->closed)
    bro_test_fail (failures, id, "not closed");
  {
    g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
    g_autofree char *want = hb_expected (bro_json_value_member (expect, "left"), FALSE), *got = NULL;
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name (d)) != NULL)
      g_ptr_array_add (names, g_strdup (name));
    if (d)
      g_dir_close (d);
    g_ptr_array_sort (names, (GCompareFunc) by_text);
    got = hb_joined (names);
    bro_test_same_string (failures, id, "left", want, got);
  }
  return TRUE;
}

static void
test_data_imager (void)
{
  bro_test_run_cases ("adapters/data-imager.cases.json", data_imager_case);
}
