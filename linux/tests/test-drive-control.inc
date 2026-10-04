/* test-drive-control.inc: BroPlatformDriveControl, included by test-adapters.c:
 * shared/fixtures/adapters/drive-control.cases.json, and the empty drives here (no eject or tray: real drives are the
 * owner's to test). */

static gboolean
drive_control_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroPlatformProcessLauncher) launcher = bro_platform_process_launcher_new ();
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  g_autoptr (BroPlatformDriveControl) drives = bro_platform_drive_control_new (BRO_PROCESS_LAUNCHER (launcher), BRO_CLOCK (clock));
  const char *open = bro_json_value_get_string (bro_json_value_member (given, "openRaw"), NULL);
  const char *probe = bro_json_value_get_string (bro_json_value_member (given, "probeContent"), NULL);
  if (open)
    {
      g_autofree char *path = bro_test_fixture_path (open);
      g_autoptr (BroBroError) error = NULL;
      g_autoptr (BroSectorReader) reader = bro_platform_drive_control_open_raw (drives, path, &error);
      BroJsonValue *reads = bro_json_value_member (expect, "reads");
      if (!reader)
        {
          bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (expect, "error"), NULL), error->code);
          return TRUE;
        }
      if (bro_sector_reader_sector_count (reader, NULL) != bro_json_value_get_integer (bro_json_value_member (expect, "sectorCount"), -1))
        bro_test_fail (failures, id, "sectorCount");
      for (guint i = 0; i < bro_json_value_length (reads); i++)
        {
          BroJsonValue *r = bro_json_value_at (reads, i);
          g_autoptr (GBytes) bytes = bro_sector_reader_read (reader, bro_json_value_get_integer (bro_json_value_member (r, "sector"), 0),
                                                             (int) bro_json_value_get_integer (bro_json_value_member (r, "count"), 0), NULL);
          const char *hex = bro_json_value_get_string (bro_json_value_member (r, "startsHex"), NULL);
          gsize n = bytes ? g_bytes_get_size (bytes) : 0;
          if ((gint64) n != bro_json_value_get_integer (bro_json_value_member (r, "length"), -1))
            bro_test_fail (failures, id, "read %u: %" G_GSIZE_FORMAT " bytes", i, n);
          if (hex)
            {
              g_autoptr (GString) got = g_string_new (NULL);
              const guint8 *data = g_bytes_get_data (bytes, NULL);
              for (gsize k = 0; k < n && k < strlen (hex) / 2; k++)
                g_string_append_printf (got, "%02x", data[k]);
              bro_test_same_string (failures, id, "start", hex, got->str);
            }
        }
      bro_sector_reader_close (reader);
    }
  else if (probe)
    {
      g_autofree char *path = bro_test_fixture_path (probe);
      bro_test_same_string (failures, id, "content", bro_json_value_get_string (bro_json_value_member (expect, "content"), NULL),
                            bro_disc_content_to_wire (bro_platform_drive_control_probe_content (drives, path)));
    }
  else
    {
      g_autofree char *path = bro_test_fixture_path (bro_json_value_get_string (bro_json_value_member (given, "waitForMount"), ""));
      gint64 t0 = g_get_monotonic_time ();
      g_autofree char *mount = bro_platform_drive_control_wait_for_mount (
        drives, path, (BroDuration) { bro_json_value_get_number (bro_json_value_member (given, "timeout"), 0) }, NULL);
      if (mount)
        bro_test_fail (failures, id, "mounted at %s", mount);
      if (since (t0) >= bro_json_value_get_number (bro_json_value_member (expect, "within"), 0))
        bro_test_fail (failures, id, "too slow");
    }
  return TRUE;
}

static void
test_drive_control (void)
{
  bro_test_run_cases ("adapters/drive-control.cases.json", drive_control_case);
}

/* Each empty optical drive here (the Ubuntu VM's virtual one): unknown content, never mounted within a second. */
static void
test_drive_control_empty_drives (void)
{
  g_autoptr (BroSystemClock) clock = bro_system_clock_new ();
  g_autoptr (BroPlatformProcessLauncher) launcher = bro_platform_process_launcher_new ();
  g_autoptr (BroPlatformDriveControl) drives = bro_platform_drive_control_new (BRO_PROCESS_LAUNCHER (launcher), BRO_CLOCK (clock));
  g_autoptr (BroPlatformDeviceMonitor) monitor = bro_platform_device_monitor_new (BRO_CLOCK (clock), 0);
  g_autoptr (GPtrArray) states = bro_platform_device_monitor_snapshot (monitor);
  for (guint i = 0; i < states->len; i++)
    {
      BroOsDriveState *s = states->pdata[i];
      g_autofree char *mount = NULL;
      if (s->media)
        continue;
      g_assert_cmpint (bro_platform_drive_control_probe_content (drives, s->drive->device), ==, BRO_DISC_CONTENT_UNKNOWN);
      mount = bro_platform_drive_control_wait_for_mount (drives, s->drive->device, (BroDuration) { 1 }, NULL);
      g_assert_null (mount);
      g_test_message ("%s: empty, unknown, not mounted", s->drive->device);
    }
}
