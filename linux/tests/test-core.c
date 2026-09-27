/* test-core.c — unit tests for the platform-neutral core, using the shared fixtures.
 * Set BROMELIA_TEST_ISO to additionally run an end-to-end rip against a real disc image. */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <json-glib/json-glib.h>

#include "bro-config.h"
#include "bro-logic.h"
#include "bro-makemkv.h"
#include "bro-robot.h"
#include "bro-runner.h"
#include "bro-state.h"

static char *
fixture (const char *name)
{
  const char *dir = g_getenv ("BROMELIA_FIXTURES");
  g_autofree char *path = g_strdup_printf ("%s/%s.txt", dir ? dir : "../../shared/fixtures", name);
  char *text = NULL;
  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  return text;
}

/* ---- robot protocol ---- */

static void
test_split_fields (void)
{
  g_autoptr (GPtrArray) f = bro_split_fields ("1,2,\"a, b\",\"say \\\"hi\\\"\",\"back\\\\slash\",plain");
  g_assert_cmpuint (f->len, ==, 6);
  g_assert_cmpstr (f->pdata[2], ==, "a, b");
  g_assert_cmpstr (f->pdata[3], ==, "say \"hi\"");
  g_assert_cmpstr (f->pdata[4], ==, "back\\slash");
  g_assert_cmpstr (f->pdata[5], ==, "plain");
}

static void
test_parse_events (void)
{
  g_autoptr (BroEvent) m = bro_event_parse ("MSG:5036,260,1,\"Copy complete. 1 titles saved.\",\"Copy complete. %1 titles saved.\",\"1\"");
  g_autoptr (BroEvent) v = bro_event_parse ("PRGV:5957,356,65536");
  g_autoptr (BroEvent) t = bro_event_parse ("PRGT:5018,0,\"Scanning CD-ROM devices\"");
  g_autoptr (BroEvent) c = bro_event_parse ("TCOUNT:3");
  g_autoptr (BroEvent) r = bro_event_parse ("Backup source must start with \"disc:\"");
  g_autoptr (BroEvent) bad = bro_event_parse ("MSG:x,y");
  g_assert_cmpint (m->type, ==, BRO_EV_MESSAGE);
  g_assert_cmpint (m->code, ==, 5036);
  g_assert_cmpuint (m->params->len, ==, 1);
  g_assert_cmpint (bro_event_severity (m), ==, BRO_SEV_INFO);
  g_assert_cmpint (v->type, ==, BRO_EV_PROGRESS_VALUE);
  g_assert_cmpint (v->current, ==, 5957);
  g_assert_cmpint (v->max, ==, 65536);
  g_assert_cmpint (t->type, ==, BRO_EV_PROGRESS_TOTAL);
  g_assert_cmpstr (t->text, ==, "Scanning CD-ROM devices");
  g_assert_cmpint (c->type, ==, BRO_EV_TITLE_COUNT);
  g_assert_cmpint (c->total, ==, 3);
  g_assert_cmpint (r->type, ==, BRO_EV_RAW);
  g_assert_cmpint (bad->type, ==, BRO_EV_RAW);
  g_assert_cmpstr (bad->text, ==, "MSG:x,y");
  g_assert_null (bro_event_parse (""));
  g_assert_cmpint (bro_message_severity (1003, 16777248, "DEBUG: Code 0"), ==, BRO_SEV_DEBUG);
  g_assert_cmpint (bro_message_severity (5037, 516, "x"), ==, BRO_SEV_ERROR);
  g_assert_cmpint (bro_message_severity (1, 1028, "x"), ==, BRO_SEV_WARNING);
}

static void
test_drive_scan_fixture (void)
{
  g_autofree char *text = fixture ("drive-scan");
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  g_autoptr (GPtrArray) present = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_entry_free);
  int total = 0;
  for (int i = 0; lines[i]; i++)
    {
      g_autoptr (BroEvent) ev = bro_event_parse (lines[i]);
      if (ev && ev->type == BRO_EV_DRIVE)
        {
          total++;
          if (bro_drive_entry_present (&ev->drive))
            g_ptr_array_add (present, bro_drive_entry_copy (&ev->drive));
        }
    }
  g_assert_cmpint (total, ==, 8);
  g_assert_cmpuint (present->len, ==, 5);
  BroDriveEntry *e0 = present->pdata[0], *e4 = present->pdata[4];
  g_assert_cmpint (e0->state, ==, BRO_DRIVE_INSERTED);
  g_assert_cmpstr (bro_disc_type_name (e0->flags), ==, "Blu-ray (AACS)");
  g_autofree char *lane = bro_drive_entry_lane (e0);
  g_assert_cmpstr (lane, ==, "dev:/dev/rdisk4");
  g_assert_cmpint (((BroDriveEntry *) present->pdata[2])->state, ==, BRO_DRIVE_EMPTY_OPEN);
  g_assert_cmpstr (e4->drive_name, ==, "DVD+RW Some \"Quoted\" Drive");
  g_assert_cmpstr (e4->disc_name, ==, "Disc, With Comma");
  g_autofree char *model = bro_drive_short_model (e0->drive_name);
  g_assert_cmpstr (model, ==, "HL-DT-ST BD-RE WH16NS60");
}

static void
test_disc_info_fixture (void)
{
  g_autofree char *text = fixture ("info-dvd");
  g_autoptr (BroDiscInfo) info = bro_disc_info_from_output (text);
  BroTitle *t0, *t1;
  g_assert_cmpint (info->reported_title_count, ==, 3);
  g_assert_cmpuint (info->titles->len, ==, 3);
  g_assert_cmpstr (bro_disc_info_type_token (info), ==, "dvd");
  g_assert_cmpstr (bro_disc_info_name (info), ==, "One_Piece_S3_P1_D1");
  t0 = bro_disc_info_title (info, 0);
  g_assert_cmpint (bro_title_chapters (t0), ==, 50);
  g_assert_cmpint (bro_title_duration (t0), ==, 2 * 3600 + 44 * 60 + 48);
  g_assert_cmpint (bro_title_size (t0), ==, 8075685888);
  g_assert_cmpint (bro_title_source_id (t0), ==, 11);
  g_assert_cmpstr (bro_title_str (t0, BRO_ATTR_OUTPUT_FILE_NAME), ==, "B1_t00.mkv");
  t1 = bro_disc_info_title (info, 1);
  g_assert_cmpuint (t1->tracks->len, ==, 6);
  g_assert_cmpint (bro_track_kind (t1->tracks->pdata[0]), ==, BRO_TRACK_VIDEO);
  g_assert_cmpstr (bro_track_attr (t1->tracks->pdata[1], BRO_ATTR_LANG_CODE), ==, "eng");
  g_assert_true (bro_track_is_default (t1->tracks->pdata[1]));
  g_assert_cmpstr (bro_track_attr (t1->tracks->pdata[3], BRO_ATTR_LANG_CODE), ==, "jpn");
  g_assert_cmpint (bro_track_kind (t1->tracks->pdata[5]), ==, BRO_TRACK_SUBTITLE);
  g_autofree char *json = bro_disc_info_to_json (info);
  g_assert_nonnull (strstr (json, "\"outputFileName\" : \"B1_t00.mkv\""));
}

static void
test_failure_fixture (void)
{
  g_autofree char *text = fixture ("rip-failure");
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  GArray *codes = g_array_new (FALSE, FALSE, sizeof (int));
  for (int i = 0; lines[i]; i++)
    {
      g_autoptr (BroEvent) ev = bro_event_parse (lines[i]);
      if (ev && ev->type == BRO_EV_MESSAGE && bro_event_severity (ev) == BRO_SEV_ERROR)
        g_array_append_val (codes, ev->code);
    }
  g_assert_cmpuint (codes->len, ==, 3);
  g_assert_cmpint (g_array_index (codes, int, 0), ==, 2003);
  g_assert_cmpint (g_array_index (codes, int, 2), ==, 5037);
  g_array_unref (codes);
}

/* ---- title selection ---- */

static void
add_title (BroDiscInfo *info, int i, const char *dur, int ch, gint64 size, int src, const char *seg, const char *name, const char *file)
{
  g_autofree char *line = NULL;
#define T(id, value) do { g_autofree char *_l = g_strdup_printf ("TINFO:%d,%d,0,\"%s\"", i, id, value); g_autoptr (BroEvent) _e = bro_event_parse (_l); bro_disc_info_consume (info, _e); } while (0)
  g_autofree char *chs = g_strdup_printf ("%d", ch);
  g_autofree char *sizes = g_strdup_printf ("%" G_GINT64_FORMAT, size);
  g_autofree char *srcs = g_strdup_printf ("%d", src);
  T (BRO_ATTR_DURATION, dur);
  T (BRO_ATTR_CHAPTER_COUNT, chs);
  T (BRO_ATTR_DISK_SIZE_BYTES, sizes);
  if (src >= 0) T (BRO_ATTR_ORIGINAL_TITLE_ID, srcs);
  if (seg) T (BRO_ATTR_SEGMENTS_MAP, seg);
  if (name) T (BRO_ATTR_NAME, name);
  if (file) T (BRO_ATTR_SOURCE_FILE_NAME, file);
#undef T
}

static BroDiscInfo *
sample_disc (void)
{
  BroDiscInfo *info = bro_disc_info_new ();
  add_title (info, 0, "1:58:02", 24, 30000000000LL, 800, "1,2,3", NULL, "00800.mpls");
  add_title (info, 1, "1:58:02", 24, 30000000000LL, 801, "1,2,3", NULL, "00801.mpls");
  add_title (info, 2, "0:22:10", 5, 2000000000LL, 10, NULL, NULL, "00010.mpls");
  add_title (info, 3, "0:23:15", 6, 2100000000LL, 11, NULL, NULL, "00011.mpls");
  add_title (info, 4, "0:03:00", 1, 100000000LL, 20, NULL, "Trailer", "00020.mpls");
  return info;
}

static char *
selected_str (BroDiscInfo *info, const BroTitleSelection *rule)
{
  g_autoptr (BroSelectionResult) r = bro_select_titles (info, rule);
  g_autoptr (GArray) sel = bro_selection_result_selected (r);
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < sel->len; i++)
    g_string_append_printf (s, "%s%d", i ? "," : "", g_array_index (sel, int, i));
  if (r->error)
    g_string_append (s, "!error");
  return g_string_free (s, FALSE);
}

static BroTitleSelection
default_rule (void)
{
  BroTitleSelection r = { 0 };
  r.longest_count = 1;
  r.index_pattern = "";
  r.include_pattern = "";
  r.exclude_pattern = "";
  r.skip_duplicates = TRUE;
  return r;
}

#define ASSERT_SEL(info, rule, expected) do { g_autofree char *_s = selected_str (info, rule); g_assert_cmpstr (_s, ==, expected); } while (0)

static void
test_selection (void)
{
  g_autoptr (BroDiscInfo) info = sample_disc ();
  BroTitleSelection r = default_rule ();
  ASSERT_SEL (info, &r, "0,2,3,4");
  {
    g_autoptr (BroSelectionResult) res = bro_select_titles (info, &r);
    g_assert_cmpstr (bro_selection_result_reason (res, 1), ==, "Duplicate of title 0");
  }
  r.skip_duplicates = FALSE;
  ASSERT_SEL (info, &r, "0,1,2,3,4");
  r = default_rule ();
  r.strategy = BRO_STRATEGY_LONGEST;
  ASSERT_SEL (info, &r, "0");
  r.longest_count = 3;
  ASSERT_SEL (info, &r, "0,2,3");
  r = default_rule ();
  r.min_duration_seconds = 20 * 60;
  r.max_duration_seconds = 30 * 60;
  ASSERT_SEL (info, &r, "2,3");
  r = default_rule ();
  r.skip_duplicates = FALSE;
  r.include_pattern = "0080[01]\\.mpls";
  ASSERT_SEL (info, &r, "0,1");
  r.include_pattern = "";
  r.exclude_pattern = "trailer";
  ASSERT_SEL (info, &r, "0,1,2,3");
  r = default_rule ();
  r.max_titles = 2;
  ASSERT_SEL (info, &r, "0,2");
  r = default_rule ();
  r.strategy = BRO_STRATEGY_INDICES;
  r.skip_duplicates = FALSE;
  r.index_pattern = "0, 3-";
  ASSERT_SEL (info, &r, "0,3,4");
  r.index_pattern = "last";
  ASSERT_SEL (info, &r, "4");
  r.index_base = BRO_INDEX_SOURCE;
  r.index_pattern = "800-801,11";
  ASSERT_SEL (info, &r, "0,1,3");
  r.index_pattern = "x";
  ASSERT_SEL (info, &r, "!error");
  r = default_rule ();
  r.strategy = BRO_STRATEGY_MANUAL;
  {
    g_autoptr (BroSelectionResult) res = bro_select_titles (info, &r);
    g_assert_true (res->requires_manual);
  }
}

/* ---- templates, arguments ---- */

static void
test_templates (void)
{
  g_autoptr (GHashTable) v = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (v, "disc", "MOVIE");
  g_hash_table_insert (v, "n", "3");
  g_hash_table_insert (v, "comment", "");
  g_hash_table_insert (v, "title", "A/B: C");
#define R(t, e) do { g_autofree char *_r = bro_template_render (t, v, FALSE); g_assert_cmpstr (_r, ==, e); } while (0)
  R ("{disc} - {n:2}", "MOVIE - 03");
  R ("{disc}{comment? ({comment})}", "MOVIE");
  R ("{disc}{n? #{n}}", "MOVIE #3");
  R ("{unknown}", "{unknown}");
#undef R
  g_autofree char *p1 = bro_template_render_path ("Movies/{title}", v);
  g_autofree char *p2 = bro_template_render_path ("../{disc}/./", v);
  g_autofree char *s1 = bro_sanitize_component ("a:b/c\\d*e?f\"g<h>i|j");
  g_autofree char *s2 = bro_sanitize_component ("  .name. ");
  g_assert_cmpstr (p1, ==, "Movies/A-B- C");
  g_assert_cmpstr (p2, ==, "MOVIE");
  g_assert_cmpstr (s1, ==, "a-b-c-d-e-f-g-h-i-j");
  g_assert_cmpstr (s2, ==, "name");
}

static void
test_arguments (void)
{
  g_autoptr (GPtrArray) a = bro_split_arguments ("-i \"my file.mkv\" --x='a b' c\\ d");
  g_assert_cmpuint (a->len, ==, 4);
  g_assert_cmpstr (a->pdata[1], ==, "my file.mkv");
  g_assert_cmpstr (a->pdata[2], ==, "--x=a b");
  g_assert_cmpstr (a->pdata[3], ==, "c d");
  g_autoptr (GPtrArray) e = bro_split_arguments ("   ");
  g_assert_cmpuint (e->len, ==, 0);

  BroPostStep *step = bro_post_step_new ();
  g_free (step->executable);
  step->executable = g_strdup ("/bin/echo");
  g_free (step->arguments);
  step->arguments = g_strdup ("--dir {outputDir} {files} \"{disc} done\"");
  g_autoptr (GHashTable) v = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (v, "outputDir", "/out/My Disc");
  g_hash_table_insert (v, "disc", "My Disc");
  g_autoptr (GPtrArray) files = g_ptr_array_new ();
  g_ptr_array_add (files, "/out/a.mkv");
  g_ptr_array_add (files, "/out/b c.mkv");
  g_autoptr (GPtrArray) argv = bro_post_step_argv (step, v, files);
  g_assert_cmpuint (argv->len, ==, 6);
  g_assert_cmpstr (argv->pdata[0], ==, "/bin/echo");
  g_assert_cmpstr (argv->pdata[2], ==, "/out/My Disc");
  g_assert_cmpstr (argv->pdata[4], ==, "/out/b c.mkv");
  g_assert_cmpstr (argv->pdata[5], ==, "My Disc done");
  g_assert_true (bro_post_step_should_run (step, BRO_JOB_SUCCEEDED));
  g_assert_false (bro_post_step_should_run (step, BRO_JOB_FAILED));
  step->run_on = BRO_RUN_FAILURE;
  g_assert_true (bro_post_step_should_run (step, BRO_JOB_CANCELLED));
  bro_post_step_free (step);
}

/* ---- files, environment ---- */

static void
test_settings_conf (void)
{
  g_autoptr (GHashTable) s = bro_settings_conf_parse ("#\n# x\n#\napp_DestinationDir = \"/Users/me/Movies\"\napp_ExpertMode = \"1\"\n");
  g_assert_cmpstr (g_hash_table_lookup (s, "app_DestinationDir"), ==, "/Users/me/Movies");
  g_autofree char *text = bro_settings_conf_serialize (s, NULL);
  g_autoptr (GHashTable) back = bro_settings_conf_parse (text);
  g_assert_cmpuint (g_hash_table_size (back), ==, 2);
  g_assert_cmpstr (g_hash_table_lookup (back, "app_ExpertMode"), ==, "1");
}

static void
test_profile (void)
{
  g_autoptr (BroDriveConfig) d = bro_drive_config_new ();
  g_free (d->profile.generated.name);
  d->profile.generated.name = g_strdup ("Test & <P>");
  g_free (d->profile.generated.selection_rule);
  d->profile.generated.selection_rule = g_strdup ("-sel:all,+sel:(eng|jpn)&audio");
  g_autofree char *xml = bro_profile_build (&d->profile.generated, NULL);
  g_assert_nonnull (strstr (xml, "app_DefaultSelectionString=\"-sel:all,+sel:(eng|jpn)&amp;audio\""));
  g_assert_nonnull (strstr (xml, "outputSettingsName=\"flac-best\""));
  g_assert_nonnull (strstr (xml, "Test &amp; &lt;P&gt;"));
}

static void
test_environment (void)
{
  g_autoptr (BroAppConfig) cfg = bro_app_config_new ();
  g_autoptr (BroDriveConfig) d = bro_drive_config_new ();
  g_autofree char *home = g_dir_make_tmp ("bromelia-test-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_free (cfg->registration_key);
  cfg->registration_key = g_strdup ("T-KEY");
  g_hash_table_insert (cfg->global_settings, g_strdup ("dvd_MinimumTitleLength"), g_strdup ("120"));
  g_hash_table_insert (cfg->global_settings, g_strdup ("app_Proxy"), g_strdup (""));
  g_hash_table_insert (d->settings, g_strdup ("dvd_MinimumTitleLength"), g_strdup ("30"));
  d->profile.mode = BRO_PROFILE_GENERATED;
  d->rip.min_length_seconds = 45;
  d->rip.cache_mb = 256;
  d->rip.direct_io = 0;
  g_autoptr (BroMakemkvEnv) env = bro_makemkv_env_prepare ("/bin/echo", cfg, d, home, "+sel:all", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (g_hash_table_lookup (env->settings, "dvd_MinimumTitleLength"), ==, "30");
  g_assert_cmpstr (g_hash_table_lookup (env->settings, "app_Key"), ==, "T-KEY");
  g_assert_cmpstr (g_hash_table_lookup (env->settings, "app_DefaultSelectionString"), ==, "+sel:all");
  g_assert_null (g_hash_table_lookup (env->settings, "app_Proxy"));
  g_assert_true (g_file_test (env->profile_path, G_FILE_TEST_EXISTS));
  g_auto (GStrv) envp = bro_makemkv_env_environ (env);
  g_assert_cmpstr (g_environ_getenv (envp, "HOME"), ==, home);

  g_autoptr (BroSource) src = bro_source_new_drive (2, "/dev/sr1");
  g_autoptr (GPtrArray) info = bro_makemkv_info_args (env, src, &d->rip);
  g_assert_cmpstr (info->pdata[info->len - 1], ==, "dev:/dev/sr1");
  g_assert_cmpstr (info->pdata[info->len - 2], ==, "info");
  gboolean has_min = FALSE, has_cache = FALSE, has_dio = FALSE;
  for (guint i = 0; i < info->len; i++)
    {
      has_min |= g_str_equal (info->pdata[i], "--minlength=45");
      has_cache |= g_str_equal (info->pdata[i], "--cache=256");
      has_dio |= g_str_equal (info->pdata[i], "--directio=false");
    }
  g_assert_true (has_min && has_cache && has_dio);
  g_autoptr (GPtrArray) bk = bro_makemkv_backup_args (env, src, TRUE, "/bk", NULL);
  g_assert_cmpstr (bk->pdata[bk->len - 2], ==, "disc:2");
  g_assert_cmpstr (bk->pdata[bk->len - 3], ==, "--decrypt");
  g_autoptr (BroSource) folder = bro_source_new_path (BRO_SOURCE_FOLDER, "/x");
  g_assert_null (bro_makemkv_backup_args (env, folder, FALSE, "/bk", NULL));
}

/* ---- configuration ---- */

static void
test_config_json (void)
{
  const char *json =
    "{ \"outputRoot\": \"/rips\", \"drives\": [ { \"id\": \"45C805B7-47BE-4674-A759-A6EAEA53521A\", \"name\": \"Left\","
    "  \"match\": { \"driveName\": \"BD-RE  HL-DT-ST\" },"
    "  \"rip\": { \"mode\": \"backupThenMkv\", \"keepBackupAfterMKV\": false, \"directIO\": true,"
    "           \"titleSelection\": { \"strategy\": \"longest\", \"indexBase\": \"source\" } },"
    "  \"profile\": { \"mode\": \"generated\", \"generated\": { \"lpcmMultichannel\": \"flac-fast\", \"useISO639Type2T\": true } },"
    "  \"postProcess\": [ { \"executable\": \"/bin/true\", \"runOn\": \"always\", \"environment\": { \"A\": \"{disc}\" } } ],"
    "  \"futureField\": 1 } ] }";
  g_autoptr (GError) error = NULL;
  g_autoptr (BroAppConfig) cfg = bro_app_config_parse (json, &error);
  BroDriveConfig *d;
  g_assert_no_error (error);
  g_assert_cmpstr (cfg->output_root, ==, "/rips");
  g_assert_cmpint (cfg->poll_interval_seconds, ==, 10);
  g_assert_cmpuint (cfg->drives->len, ==, 1);
  d = cfg->drives->pdata[0];
  g_assert_cmpstr (d->id, ==, "45C805B7-47BE-4674-A759-A6EAEA53521A");
  g_assert_cmpint (d->rip.mode, ==, BRO_MODE_BACKUP_THEN_MKV);
  g_assert_false (d->rip.keep_backup_after_mkv);
  g_assert_cmpint (d->rip.direct_io, ==, 1);
  g_assert_cmpint (d->rip.min_length_seconds, ==, -1);
  g_assert_cmpint (d->rip.titles.strategy, ==, BRO_STRATEGY_LONGEST);
  g_assert_cmpint (d->rip.titles.index_base, ==, BRO_INDEX_SOURCE);
  g_assert_true (d->rip.titles.skip_duplicates);
  g_assert_cmpint (d->profile.generated.lpcm_multichannel, ==, BRO_LPCM_FLAC_FAST);
  g_assert_true (d->profile.generated.use_iso639_2t);
  g_assert_cmpint (((BroPostStep *) d->post_process->pdata[0])->run_on, ==, BRO_RUN_ALWAYS);
  g_assert_true (d->automation.eject_when_done);

  /* Round trip. */
  g_autofree char *text = bro_app_config_serialize (cfg);
  g_assert_nonnull (strstr (text, "\"mode\" : \"backupThenMkv\""));
  g_assert_nonnull (strstr (text, "\"lpcmMultichannel\" : \"flac-fast\""));
  g_assert_null (strstr (text, "minLengthSeconds"));
  g_autoptr (BroAppConfig) back = bro_app_config_parse (text, &error);
  g_autofree char *text2 = bro_app_config_serialize (back);
  g_assert_cmpstr (text, ==, text2);

  /* Matching. */
  BroDriveEntry e = { 0, BRO_DRIVE_INSERTED, 0, "bd-re hl-dt-st", "", "/dev/sr0" };
  g_assert_true (bro_drive_config_matches (d, &e));
  g_assert_true (bro_app_config_drive_for_entry (cfg, &e) == d);
}

static void
test_remux_args (void)
{
  g_autofree char *text = fixture ("info-dvd");
  g_autoptr (BroDiscInfo) info = bro_disc_info_from_output (text);
  BroTitle *t = bro_disc_info_title (info, 1);
  g_autoptr (GPtrArray) layout = g_ptr_array_new ();
  const char *types[] = { "video", "audio", "audio", "audio", "audio", "subtitles" };
  g_autoptr (GHashTable) keep = bro_int_set_new ();
  for (int i = 0; i < 6; i++)
    g_ptr_array_add (layout, (char *) types[i]);
  g_hash_table_add (keep, GINT_TO_POINTER (0));
  g_hash_table_add (keep, GINT_TO_POINTER (1));
  g_hash_table_add (keep, GINT_TO_POINTER (3));
  g_autoptr (GPtrArray) args = bro_remux_arguments ("mkvmerge", layout, t, keep, "in.mkv", "out.mkv");
  g_ptr_array_add (args, NULL);
  g_autofree char *joined = g_strjoinv (" ", (char **) args->pdata);
  g_assert_cmpstr (joined, ==, "mkvmerge -o out.mkv --video-tracks 0 --audio-tracks 1,3 --no-subtitles in.mkv");
  g_ptr_array_set_size (layout, 5);
  g_assert_null (bro_remux_arguments ("mkvmerge", layout, t, keep, "in.mkv", "out.mkv"));
}

/* ---- drive detection ---- */

static BroDriveEntry *
entry (int index, BroDriveState state, const char *name, const char *dev, const char *disc)
{
  BroDriveEntry e = { index, state, state == BRO_DRIVE_INSERTED ? BRO_DISC_BLURAY : 0, (char *) name, (char *) disc, (char *) dev };
  return bro_drive_entry_copy (&e);
}

static void
scan (BroState *st, BroDriveEntry *a, BroDriveEntry *b)
{
  g_autoptr (GPtrArray) arr = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_entry_free);
  if (a) g_ptr_array_add (arr, a);
  if (b) g_ptr_array_add (arr, b);
  bro_state_apply_scan (st, arr);
}

static void
test_auto_rip (void)
{
  g_autoptr (BroState) st = bro_state_new (NULL);
  BroDriveConfig *left = bro_drive_config_new ();
  const char *name = "BD-RE  TEST DRIVE 1.00 SERIAL1";
  BroJob *job;
  g_free (left->name);
  left->name = g_strdup ("Left");
  g_free (left->match_drive_name);
  left->match_drive_name = g_strdup ("BD-RE TEST DRIVE 1.00 SERIAL1");
  left->automation.auto_rip_on_insert = TRUE;
  left->automation.auto_rip_delay_seconds = 60;
  left->rip.mode = BRO_MODE_BACKUP_THEN_MKV;
  g_ptr_array_add (st->config->drives, left);

  scan (st, entry (0, BRO_DRIVE_INSERTED, name, "/dev/sr0", "OLD"), entry (1, BRO_DRIVE_EMPTY_CLOSED, "DVD OTHER 2.00", "/dev/sr1", ""));
  g_assert_cmpuint (st->jobs->len, ==, 0);
  scan (st, entry (0, BRO_DRIVE_EMPTY_CLOSED, name, "/dev/sr0", ""), entry (1, BRO_DRIVE_EMPTY_CLOSED, "DVD OTHER 2.00", "/dev/sr1", ""));
  g_assert_cmpuint (st->jobs->len, ==, 0);
  scan (st, entry (0, BRO_DRIVE_INSERTED, name, "/dev/sr0", "MOVIE"), entry (1, BRO_DRIVE_EMPTY_CLOSED, "DVD OTHER 2.00", "/dev/sr1", ""));
  g_assert_cmpuint (st->jobs->len, ==, 1);
  job = st->jobs->pdata[0];
  g_assert_cmpint (job->state, ==, BRO_JOB_WAITING);
  g_assert_true (job->automatic);
  g_assert_cmpint (job->mode, ==, BRO_MODE_BACKUP_THEN_MKV);
  g_assert_cmpstr (job->drive->name, ==, "Left");
  g_assert_cmpstr (job->lane, ==, "dev:/dev/sr0");
  scan (st, entry (0, BRO_DRIVE_INSERTED, name, "/dev/sr0", "MOVIE"), entry (1, BRO_DRIVE_INSERTED, "DVD OTHER 2.00", "/dev/sr1", "X"));
  g_assert_cmpuint (st->jobs->len, ==, 1);
  {
    g_autoptr (GPtrArray) items = bro_state_drive_items (st);
    g_assert_cmpuint (items->len, ==, 2);
    g_assert_true (((BroDriveItem *) items->pdata[0])->config == left);
    g_assert_null (((BroDriveItem *) items->pdata[1])->config);
  }
  bro_state_cancel_job (st, job);
  g_assert_cmpint (job->state, ==, BRO_JOB_CANCELLED);
  g_assert_cmpstr (((BroHistoryRecord *) st->history->pdata[0])->id, ==, job->id);
}

/* ---- integration ---- */

static void
collect (const char *line, gpointer data)
{
  g_string_append_printf (data, "%s\n", line);
}

static void
test_integration (void)
{
  const char *iso = g_getenv ("BROMELIA_TEST_ISO");
  g_autofree char *exe = bro_find_tool ("", "makemkvcon");
  g_autofree char *mkvmerge = bro_find_tool ("", "mkvmerge");
  g_autofree char *out = NULL;
  g_autofree char *marker = NULL;
  BroRunRequest *req;
  BroRunResult *res;
  BroPostStep *step;
  GHashTable *keep;

  if (!iso || !exe)
    {
      g_test_skip ("set BROMELIA_TEST_ISO and install makemkvcon to run");
      return;
    }
  out = g_dir_make_tmp ("bromelia-it-XXXXXX", NULL);
  marker = g_build_filename (out, "post.txt", NULL);
  req = bro_run_request_new ();
  req->job_id = g_uuid_string_random ();
  req->job_dir = g_build_filename (out, ".job", NULL);
  req->config = bro_app_config_new ();
  g_free (req->config->output_root);
  req->config->output_root = g_strdup (out);
  req->drive = bro_drive_config_new ();
  g_free (req->drive->output.folder_template);
  req->drive->output.folder_template = g_strdup ("{type}/{disc}");
  g_free (req->drive->output.file_name_template);
  req->drive->output.file_name_template = g_strdup ("{disc} - {n:2}");
  step = bro_post_step_new ();
  g_free (step->executable);
  step->executable = g_strdup ("/bin/sh");
  g_free (step->arguments);
  step->arguments = g_strdup_printf ("-c 'echo \"$BROMELIA_STATUS|$BROMELIA_FILE_COUNT\" > \"$0\"' %s", marker);
  g_ptr_array_add (req->drive->post_process, step);
  req->source = bro_source_new_path (BRO_SOURCE_ISO, iso);
  req->mode = BRO_MODE_MKV;
  req->manual_titles = g_array_new (FALSE, FALSE, sizeof (int));
  int one = 1;
  g_array_append_val (req->manual_titles, one);
  req->makemkvcon = g_strdup (exe);
  req->mkvmerge = g_strdup (mkvmerge);
  if (mkvmerge)
    {
      keep = bro_int_set_new ();
      g_hash_table_add (keep, GINT_TO_POINTER (0));
      g_hash_table_add (keep, GINT_TO_POINTER (1));
      g_hash_table_add (keep, GINT_TO_POINTER (5));
      g_hash_table_insert (req->track_selections, GINT_TO_POINTER (1), keep);
    }
  res = bro_run_job (req);
  if (res->status != BRO_JOB_SUCCEEDED)
    g_printerr ("job failed: %s\n", res->error);
  g_assert_cmpint (res->status, ==, BRO_JOB_SUCCEEDED);
  g_assert_cmpuint (res->files->len, ==, 1);
  {
    g_autofree char *base = g_path_get_basename (res->files->pdata[0]);
    g_autofree char *expected = g_strdup_printf ("%s - 01.mkv", res->disc_label);
    g_assert_cmpstr (base, ==, expected);
    g_assert_true (g_file_test (res->files->pdata[0], G_FILE_TEST_EXISTS));
  }
  {
    g_autofree char *post = NULL;
    g_assert_true (g_file_get_contents (marker, &post, NULL, NULL));
    g_assert_true (g_str_has_prefix (post, "success|1"));
  }
  if (mkvmerge)
    {
      const char *argv[] = { mkvmerge, "-J", res->files->pdata[0], NULL };
      GString *json = g_string_new (NULL);
      int status = -1;
      g_autoptr (JsonParser) parser = json_parser_new ();
      g_assert_true (bro_process_run (argv, NULL, NULL, 60, NULL, collect, json, &status, NULL, NULL, NULL));
      g_assert_cmpint (status, ==, 0);
      g_assert_true (json_parser_load_from_data (parser, json->str, -1, NULL));
      JsonArray *tracks = json_object_get_array_member (json_node_get_object (json_parser_get_root (parser)), "tracks");
      g_assert_cmpuint (json_array_get_length (tracks), ==, 3);
      g_string_free (json, TRUE);
    }
  bro_run_result_free (res);
  bro_run_request_free (req);
  {
    const char *argv[] = { "/bin/rm", "-rf", out, NULL };
    bro_process_run (argv, NULL, NULL, 60, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  {
    /* Keep the state tests away from the real configuration and data folders. */
    g_autofree char *tmp = g_dir_make_tmp ("bromelia-test-home-XXXXXX", NULL);
    g_autofree char *cfg = g_build_filename (tmp, "config", NULL);
    g_autofree char *data = g_build_filename (tmp, "data", NULL);
    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_setenv ("XDG_DATA_HOME", data, TRUE);
  }
  g_test_add_func ("/robot/split-fields", test_split_fields);
  g_test_add_func ("/robot/parse-events", test_parse_events);
  g_test_add_func ("/robot/drive-scan-fixture", test_drive_scan_fixture);
  g_test_add_func ("/robot/disc-info-fixture", test_disc_info_fixture);
  g_test_add_func ("/robot/failure-fixture", test_failure_fixture);
  g_test_add_func ("/logic/selection", test_selection);
  g_test_add_func ("/logic/templates", test_templates);
  g_test_add_func ("/logic/arguments", test_arguments);
  g_test_add_func ("/makemkv/settings-conf", test_settings_conf);
  g_test_add_func ("/makemkv/profile", test_profile);
  g_test_add_func ("/makemkv/environment", test_environment);
  g_test_add_func ("/config/json", test_config_json);
  g_test_add_func ("/runner/remux-args", test_remux_args);
  g_test_add_func ("/state/auto-rip", test_auto_rip);
  g_test_add_func ("/runner/integration", test_integration);
  return g_test_run ();
}
