/* test-core.c — unit tests for the platform-neutral core, using the shared fixtures.
 * Set BROMELIA_TEST_ISO to additionally run an end-to-end rip against a real disc image. */
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <json-glib/json-glib.h>

#include "bro-config.h"
#include "bro-dvd.h"
#include "bro-identity.h"
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

/* ---- identity, naming, plugins, checksums, DVD navigation ---- */

static void
test_labels (void)
{
  static const struct { const char *label, *title; int season, part, volume, disc; gboolean series; } cases[] = {
    { "ONE_PIECE_S2_P7_D2", "One Piece", 2, 7, -1, 2, TRUE },
    { "One_Piece_S3_P1_D1", "One Piece", 3, 1, -1, 1, TRUE },
    { "THE_LORD_OF_THE_RINGS_DISC_2", "The Lord of the Rings", -1, -1, -1, 2, FALSE },
    { "FRIENDS_SEASON_4_DISC_3", "Friends", 4, -1, -1, 3, TRUE },
    { "BREAKING_BAD_S1D2", "Breaking Bad", 1, -1, -1, 2, TRUE },
    { "NARUTO_VOL_12", "Naruto", -1, -1, 12, -1, TRUE },
    { "BLADE_RUNNER_2049", "Blade Runner 2049", -1, -1, -1, -1, FALSE },
    { "ROCKY_II_WS", "Rocky II", -1, -1, -1, -1, FALSE },
    { "SPIDER-MAN_NO_WAY_HOME", "Spider-Man No Way Home", -1, -1, -1, -1, FALSE },
    { "The Matrix - Disc 1", "The Matrix", -1, -1, -1, 1, FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      BroLabelInfo l;
      bro_label_parse (cases[i].label, &l);
      g_assert_cmpstr (l.title, ==, cases[i].title);
      g_assert_cmpint (l.season, ==, cases[i].season);
      g_assert_cmpint (l.part, ==, cases[i].part);
      g_assert_cmpint (l.volume, ==, cases[i].volume);
      g_assert_cmpint (l.disc, ==, cases[i].disc);
      g_assert_cmpint (l.looks_like_series, ==, cases[i].series);
      bro_label_clear (&l);
    }
  {
    BroLabelInfo l;
    g_autofree char *set = NULL;
    bro_label_parse ("ONE_PIECE_S2_P7_D2", &l);
    set = bro_label_set_description (&l);
    g_assert_cmpstr (set, ==, "Season 2 Part 7 Disc 2");
    bro_label_clear (&l);
  }
}

static void
test_identity (void)
{
  g_autofree char *text = fixture ("info-dvd");
  g_autoptr (BroDiscInfo) info = bro_disc_info_from_output (text);
  BroIdentity *id = bro_identity_resolve (info, "", -1, -1, FALSE, "", -1, 0);
  g_autofree char *code = bro_identity_format_code (id);
  g_autofree char *set = bro_label_set_description (&id->label);
  g_assert_cmpstr (id->name, ==, "One Piece");
  g_assert_cmpint (id->kind, ==, BRO_KIND_TV);
  g_assert_cmpstr (code, ==, "DVD");
  g_assert_cmpstr (set, ==, "Season 3 Part 1 Disc 1");
  bro_identity_free (id);
  id = bro_identity_resolve (info, "", -1, -1, TRUE, "One Piece (2001)", BRO_KIND_MOVIE, 0);
  g_free (code);
  code = bro_identity_format_code (id);
  g_assert_cmpstr (code, ==, "DVDe");
  g_assert_cmpstr (id->name, ==, "One Piece (2001)");
  g_assert_cmpint (id->kind, ==, BRO_KIND_MOVIE);
  bro_identity_free (id);

  {
    g_autoptr (BroDiscInfo) bd = bro_disc_info_from_output ("CINFO:1,6209,\"Blu-ray disc\"\nCINFO:2,0,\"The Dark Knight™\"\nCINFO:32,0,\"DARK_KNIGHT_D1\"\n"
                                                            "TINFO:0,9,0,\"2:32:00\"\nSINFO:0,0,1,6201,\"Video\"\nSINFO:0,0,19,0,\"1920x1080\"\n");
    id = bro_identity_resolve (bd, "", -1, -1, FALSE, "", -1, 0);
    g_assert_cmpstr (id->name, ==, "The Dark Knight");
    g_assert_cmpint (id->kind, ==, BRO_KIND_MOVIE);
    g_assert_cmpint (id->format, ==, BRO_FORMAT_BLURAY);
    g_assert_cmpint (id->label.disc, ==, 1);
    bro_identity_free (id);
  }
  {
    g_autoptr (BroDiscInfo) uhd = bro_disc_info_from_output ("CINFO:1,6209,\"Blu-ray disc\"\nSINFO:0,0,1,6201,\"Video\"\nSINFO:0,0,19,0,\"3840x2160\"\n");
    g_autofree char *c4 = bro_format_code (bro_detect_format (uhd, -1), TRUE);
    g_assert_cmpstr (c4, ==, "4Ke");
    g_assert_cmpint (bro_detect_format (NULL, BRO_DISC_BLURAY | BRO_DISC_AACS), ==, BRO_FORMAT_BLURAY);
    g_assert_cmpint (bro_detect_format (NULL, -1), ==, BRO_FORMAT_UNKNOWN);
  }
  {
    g_autoptr (BroDiscInfo) tv = bro_disc_info_from_output ("CINFO:1,6209,\"Blu-ray disc\"\nCINFO:32,0,\"SOME_SHOW\"\n"
                                                            "TINFO:0,9,0,\"0:45:10\"\nTINFO:1,9,0,\"0:44:10\"\nTINFO:2,9,0,\"0:44:50\"\n"
                                                            "TINFO:3,9,0,\"0:45:05\"\nTINFO:4,9,0,\"0:05:00\"\n");
    g_autoptr (GPtrArray) eps = bro_episode_like_titles (tv->titles);
    g_assert_cmpuint (eps->len, ==, 4);
    id = bro_identity_resolve (tv, "", -1, -1, FALSE, "", -1, 0);
    g_assert_cmpint (id->kind, ==, BRO_KIND_TV);
    bro_identity_free (id);
  }
  {
    g_autofree char *dir = g_dir_make_tmp ("bromelia-bdmv-XXXXXX", NULL);
    g_autofree char *bdmv = g_build_filename (dir, "BDMV", NULL);
    g_autofree char *index = g_build_filename (bdmv, "index.bdmv", NULL);
    g_mkdir_with_parents (bdmv, 0755);
    g_file_set_contents (index, "INDX0300xxxxxxxx", -1, NULL);
    g_assert_cmpint (bro_detect_backup_folder (dir), ==, BRO_FORMAT_UHD);
    g_file_set_contents (index, "INDX0200xxxxxxxx", -1, NULL);
    g_assert_cmpint (bro_detect_backup_folder (dir), ==, BRO_FORMAT_BLURAY);
    g_unlink (index);
    g_rmdir (bdmv);
    g_rmdir (dir);
  }
}

static char *
render_default (BroIdentity *id, const char *rip, const char *episode, const char *track)
{
  g_autoptr (GHashTable) v = bro_template_values_new ();
  bro_identity_template_values (id, v, rip);
  if (episode) bro_template_values_set (v, "episode", episode);
  if (track) bro_template_values_set (v, "track", track);
  return bro_template_render_path (BRO_DEFAULT_FILE_TEMPLATE, v);
}

static void
test_naming (void)
{
  BroIdentity *id = bro_identity_resolve (NULL, "ONE_PIECE_S2_P7_D2", -1, BRO_FORMAT_DVD, FALSE, "", -1, 0);
  g_autofree char *a = render_default (id, "Rip", "Episode 138", "Title 11 Ch 1-7");
  g_assert_cmpstr (a, ==, "One Piece - Episode 138 - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 1-7 - DVD");
  id->encrypted = TRUE;
  {
    g_autofree char *b = render_default (id, "Backup", NULL, NULL);
    g_assert_cmpstr (b, ==, "One Piece - Season 2 Part 7 Disc 2 - Backup - DVDe");
  }
  bro_identity_free (id);
  id = bro_identity_resolve (NULL, "INCEPTION", -1, BRO_FORMAT_UHD, FALSE, "Inception", -1, 0);
  {
    g_autofree char *c = render_default (id, "Rip", NULL, "Playlist 00800");
    g_autoptr (GHashTable) v = bro_template_values_new ();
    g_autofree char *folder = NULL;
    g_assert_cmpstr (c, ==, "Inception - Rip - Playlist 00800 - 4K");
    bro_identity_template_values (id, v, "Rip");
    folder = bro_template_render_path (BRO_DEFAULT_FOLDER_TEMPLATE, v);
    g_assert_cmpstr (folder, ==, "Inception");
  }
  bro_identity_free (id);
  {
    g_autofree char *e = bro_episode_label (7, 3);
    g_assert_cmpstr (e, ==, "Episode 007");
  }
}

static void
test_plugins (void)
{
  BroPostStep *s = bro_post_step_new ();
  g_autofree char *err = NULL;
  g_assert_true (bro_plugin_matches (s, "Anything", "X", "BR"));
  g_free (s->match_name);
  s->match_name = g_strdup ("^one piece$");
  g_assert_true (bro_plugin_matches (s, "One Piece", "ONE_PIECE_S2", "DVD"));
  g_assert_false (bro_plugin_matches (s, "Naruto", "NARUTO", "DVD"));
  g_free (s->match_name);
  s->match_name = g_strdup ("S2_P7");
  g_assert_true (bro_plugin_matches (s, "One Piece", "ONE_PIECE_S2_P7_D2", "DVD"));
  g_free (s->match_name);
  s->match_name = g_strdup ("");
  g_ptr_array_add (s->match_formats, g_strdup ("BR*"));
  g_ptr_array_add (s->match_formats, g_strdup ("4K"));
  g_assert_true (bro_plugin_matches (s, "", "", "BRe"));
  g_assert_true (bro_plugin_matches (s, "", "", "4K"));
  g_assert_false (bro_plugin_matches (s, "", "", "4Ke"));
  g_assert_false (bro_plugin_matches (s, "", "", "DVD"));
  g_free (s->match_name);
  s->match_name = g_strdup ("(");
  g_assert_false (bro_plugin_matches (s, "x", "", "BR"));
  err = bro_plugin_validate ("(");
  g_assert_nonnull (err);
  bro_post_step_free (s);
}

static void
test_config_upgrade (void)
{
  const char *v1 = "{\"version\":1,\"defaultDrive\":{\"output\":{\"folderTemplate\":\"{disc}\",\"fileNameTemplate\":\"\"}},"
                   "\"drives\":[{\"name\":\"A\",\"output\":{\"folderTemplate\":\"{type}/{disc}\",\"fileNameTemplate\":\"{disc} {n}\"}}],"
                   "\"plugins\":[{\"name\":\"P\",\"matchName\":\"piece\",\"matchFormats\":[\"DVD\"]}]}";
  g_autoptr (BroAppConfig) c = bro_app_config_parse (v1, NULL);
  BroDriveConfig *d;
  BroPostStep *p;
  g_assert_cmpint (c->version, ==, 2);
  g_assert_cmpstr (c->default_drive->output.folder_template, ==, BRO_DEFAULT_FOLDER_TEMPLATE);
  g_assert_cmpstr (c->default_drive->output.file_name_template, ==, BRO_DEFAULT_FILE_TEMPLATE);
  d = c->drives->pdata[0];
  g_assert_cmpstr (d->output.folder_template, ==, "{type}/{disc}");
  g_assert_cmpstr (d->output.file_name_template, ==, "{disc} {n}");
  p = c->plugins->pdata[0];
  g_assert_cmpstr (p->match_name, ==, "piece");
  g_assert_cmpstr (p->match_formats->pdata[0], ==, "DVD");
  g_assert_true (c->default_drive->archive.checksums);
  g_assert_true (c->default_drive->episodes.split_play_all);
  {
    g_autoptr (BroAppConfig) v2 = bro_app_config_parse ("{\"version\":2,\"defaultDrive\":{\"output\":{\"fileNameTemplate\":\"\"}}}", NULL);
    g_autofree char *json = bro_app_config_serialize (c);
    g_autoptr (BroAppConfig) back = bro_app_config_parse (json, NULL);
    g_assert_cmpstr (v2->default_drive->output.file_name_template, ==, "");
    g_assert_cmpuint (back->plugins->len, ==, 1);
    g_assert_cmpstr (((BroPostStep *) back->plugins->pdata[0])->match_formats->pdata[0], ==, "DVD");
  }
}

static void
test_checksums (void)
{
  g_autofree char *dir = g_dir_make_tmp ("bromelia-sums-XXXXXX", NULL);
  g_autofree char *vts = g_build_filename (dir, "backup", "VIDEO_TS", NULL);
  g_autofree char *a = g_build_filename (dir, "a.mkv", NULL);
  g_autofree char *ifo = g_build_filename (vts, "VIDEO_TS.IFO", NULL);
  g_autofree char *backup = g_build_filename (dir, "backup", NULL);
  g_autoptr (GPtrArray) items = g_ptr_array_new ();
  g_autoptr (GPtrArray) files = NULL;
  g_autoptr (GPtrArray) bad = NULL;
  g_autofree char *sums = NULL;
  g_mkdir_with_parents (vts, 0755);
  g_file_set_contents (a, "abc", -1, NULL);
  g_file_set_contents (ifo, "", 0, NULL);
  g_ptr_array_add (items, a);
  g_ptr_array_add (items, backup);
  files = bro_checksum_list_files (items, dir);
  g_assert_cmpuint (files->len, ==, 2);
  g_assert_cmpstr (((BroChecksum *) files->pdata[0])->path, ==, "a.mkv");
  g_assert_cmpstr (((BroChecksum *) files->pdata[1])->path, ==, "backup/VIDEO_TS/VIDEO_TS.IFO");
  for (guint i = 0; i < files->len; i++)
    {
      BroChecksum *c = files->pdata[i];
      g_autofree char *full = g_build_filename (dir, c->path, NULL);
      c->sha256 = bro_sha256_file (full, NULL, NULL, NULL);
    }
  g_assert_cmpstr (((BroChecksum *) files->pdata[0])->sha256, ==, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  sums = bro_checksums_write_merged (dir, files, NULL);
  g_assert_nonnull (sums);
  bad = bro_checksums_verify (dir, NULL);
  g_assert_cmpuint (bad->len, ==, 0);
  g_file_set_contents (a, "abd", -1, NULL);
  g_ptr_array_unref (bad);
  bad = bro_checksums_verify (dir, NULL);
  g_assert_cmpuint (bad->len, ==, 1);
  g_assert_cmpstr (bad->pdata[0], ==, "a.mkv");
  {
    const char *argv[] = { "/bin/rm", "-rf", dir, NULL };
    bro_process_run (argv, NULL, NULL, 60, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  }
}

static int
int_at (GArray *a, guint i)
{
  return g_array_index (a, int, i);
}

static void
test_dvd_navigation (void)
{
  const char *fx = g_getenv ("BROMELIA_FIXTURES");
  g_autofree char *dir = g_build_filename (fx ? fx : "../../shared/fixtures", "dvd-play-all", NULL);
  g_autoptr (BroVideoTS) r = bro_videots_open_folder (dir, "ONE_PIECE_S2_P7_D2");
  g_autoptr (BroDvdAnalysis) a = NULL;
  g_autoptr (GPtrArray) plans = NULL;
  g_autoptr (GArray) split = NULL;
  BroEpisodePlan *p;
  static const int starts[] = { 1, 8, 15, 22, 29, 36 };
  static const int splits[] = { 8, 15, 22, 29, 36, 43 };
  g_autofree char *dur = NULL, *at8 = NULL;
  int f, l;

  g_assert_nonnull (r);
  a = bro_dvd_analyse (r);
  g_assert_nonnull (a);
  plans = bro_dvd_plans (a);
  g_assert_cmpuint (plans->len, >, 0);
  p = plans->pdata[0];
  g_assert_cmpint (p->title, ==, 11);
  g_assert_cmpuint (p->starts->len, ==, 6);
  for (guint i = 0; i < 6; i++)
    g_assert_cmpint (int_at (p->starts, i), ==, starts[i]);
  g_assert_cmpint (p->last_end, ==, 42);
  g_assert_cmpstr (p->end_rule, ==, "VOB boundary");
  g_assert_cmpuint (p->tail->len, ==, 1);
  g_assert_cmpint (int_at (p->tail, 0), ==, 43);
  split = bro_episode_plan_split_chapters (p);
  for (guint i = 0; i < 6; i++)
    g_assert_cmpint (int_at (split, i), ==, splits[i]);
  dur = bro_dvd_hms (p->duration);
  at8 = bro_dvd_hms (g_array_index (p->chapter_starts, double, 7));
  g_assert_cmpstr (dur, ==, "2:21:51.370");
  g_assert_cmpstr (at8, ==, "0:23:36.815");
  bro_episode_plan_range (p, 5, &f, &l);
  g_assert_cmpint (f, ==, 36);
  g_assert_cmpint (l, ==, 42);
  g_assert_true (bro_episode_plan_plausible (p, TRUE));
  g_assert_true (bro_episode_plan_matches_chapters (p, 43));
  g_assert_cmpstr (p->reasons->pdata[1], ==, "if GPRM7 == 21: LinkPTTN 8");

  {
    g_autoptr (GArray) shifted = g_array_new (FALSE, FALSE, sizeof (double));
    g_autoptr (GArray) mapped = NULL;
    double zero = 0;
    g_array_append_val (shifted, zero);
    for (guint i = 0; i < p->chapter_starts->len; i++)
      {
        double v = g_array_index (p->chapter_starts, double, i) + 0.02;
        g_array_append_val (shifted, v);
      }
    mapped = bro_dvd_mkv_chapters (split, p, shifted);
    g_assert_nonnull (mapped);
    for (guint i = 0; i < 6; i++)
      g_assert_cmpint (int_at (mapped, i), ==, splits[i] + 1);
  }
  {
    g_autoptr (GArray) ch = bro_parse_simple_chapters ("CHAPTER01=00:00:00.000\nCHAPTER01NAME=x\nCHAPTER02=00:23:36.815\n");
    g_assert_cmpuint (ch->len, ==, 2);
    g_assert_cmpfloat_with_epsilon (g_array_index (ch, double, 1), 1416.815, 1e-9);
  }
}

static void
test_dvd_jumps_and_numbers (void)
{
  static const guint8 jvts[] = { 0x30, 0x05, 0x00, 0x05, 0x00, 0x03, 0x00, 0x00 };
  static const guint8 link[] = { 0x20, 0xA5, 0x00, 0x07, 0x00, 0x15, 0x00, 0x08 };
  static const guint8 jtt[] = { 0x30, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 };
  static const guint8 none[8] = { 0 };
  BroDvdJump j;
  g_autoptr (GArray) numbers = bro_dvd_episode_numbers ("EPISODE 138\nEpis0de #12 foo episode9");
  g_autoptr (GArray) ocr = g_array_new (FALSE, FALSE, sizeof (int));
  static const int seen[] = { 138, 139, 140, 142, 143 };

  g_assert_true (bro_dvd_decode_jump (jvts, &j));
  g_assert_true (j.chapter);
  g_assert_cmpint (j.title_number, ==, 3);
  g_assert_cmpint (j.target, ==, 5);
  g_free (j.condition);
  g_assert_true (bro_dvd_decode_jump (link, &j));
  g_assert_cmpint (j.title_number, ==, -1);
  g_assert_cmpint (j.target, ==, 8);
  g_assert_cmpstr (j.condition, ==, "if GPRM7 == 21: ");
  g_free (j.condition);
  g_assert_true (bro_dvd_decode_jump (jtt, &j));
  g_assert_false (j.chapter);
  g_assert_cmpint (j.target, ==, 4);
  g_free (j.condition);
  g_assert_false (bro_dvd_decode_jump (none, &j));

  g_assert_cmpuint (numbers->len, ==, 3);
  g_assert_cmpint (int_at (numbers, 0), ==, 138);
  g_assert_cmpint (int_at (numbers, 1), ==, 12);
  g_assert_cmpint (int_at (numbers, 2), ==, 9);
  g_array_append_vals (ocr, seen, G_N_ELEMENTS (seen));
  g_assert_cmpint (bro_dvd_first_episode (ocr, 6), ==, 138);
  g_array_set_size (ocr, 0);
  {
    int five = 5;
    g_array_append_val (ocr, five);
  }
  g_assert_cmpint (bro_dvd_first_episode (ocr, 6), ==, -1);
}

/* Reads the same disc from its ISO (BROMELIA_TEST_DVD_ISO=/path/One_Piece_S2_P7_D2.iso). */
static void
test_dvd_iso (void)
{
  const char *iso = g_getenv ("BROMELIA_TEST_DVD_ISO");
  g_autoptr (BroVideoTS) r = NULL;
  g_autoptr (BroDvdAnalysis) a = NULL;
  g_autoptr (GPtrArray) plans = NULL;
  g_autoptr (GPtrArray) stills = NULL;
  if (!iso)
    {
      g_test_skip ("set BROMELIA_TEST_DVD_ISO to run");
      return;
    }
  r = bro_videots_open_iso (iso);
  g_assert_nonnull (r);
  g_assert_cmpstr (bro_videots_label (r), ==, "ONE_PIECE_S2_P7_D2");
  a = bro_dvd_analyse (r);
  plans = bro_dvd_plans (a);
  g_assert_cmpint (((BroEpisodePlan *) plans->pdata[0])->title, ==, 11);
  g_assert_cmpuint (((BroEpisodePlan *) plans->pdata[0])->starts->len, ==, 6);
  stills = bro_dvd_menu_stills (r);
  g_assert_cmpuint (stills->len, >, 0);
}

/* Rips title 11 of One_Piece_S2_P7_D2 and checks the split into named episodes (BROMELIA_TEST_PLAYALL_ISO). */
static void
test_split_integration (void)
{
  const char *iso = g_getenv ("BROMELIA_TEST_PLAYALL_ISO");
  g_autofree char *exe = bro_find_tool ("", "makemkvcon");
  g_autofree char *mkvmerge = bro_find_tool ("", "mkvmerge");
  g_autofree char *out = NULL, *plugin_out = NULL;
  BroRunRequest *req;
  BroRunResult *res;
  BroPostStep *plugin, *other;

  if (!iso || !exe || !mkvmerge)
    {
      g_test_skip ("set BROMELIA_TEST_PLAYALL_ISO and install makemkvcon + mkvmerge to run");
      return;
    }
  out = g_dir_make_tmp ("bromelia-split-XXXXXX", NULL);
  req = bro_run_request_new ();
  req->job_id = g_uuid_string_random ();
  req->job_dir = g_build_filename (out, ".job", NULL);
  req->config = bro_app_config_new ();
  g_free (req->config->output_root);
  req->config->output_root = g_strdup (out);
  req->drive = bro_drive_config_new ();
  req->drive->rip.titles.strategy = BRO_STRATEGY_INDICES;
  req->drive->rip.titles.index_base = BRO_INDEX_SOURCE;
  g_free (req->drive->rip.titles.index_pattern);
  req->drive->rip.titles.index_pattern = g_strdup ("11");
  req->drive->episodes.keep_play_all = FALSE;
  plugin = bro_post_step_new ();
  g_free (plugin->executable);
  plugin->executable = g_strdup ("/bin/sh");
  g_free (plugin->arguments);
  plugin->arguments = g_strdup ("-c 'echo \"$BROMELIA_NAME|$BROMELIA_FORMAT|$BROMELIA_FILE_COUNT\" > plugin.txt'");
  g_free (plugin->match_name);
  plugin->match_name = g_strdup ("one piece");
  g_ptr_array_add (plugin->match_formats, g_strdup ("DVD"));
  other = bro_post_step_copy (plugin);
  g_ptr_array_set_size (other->match_formats, 0);
  g_ptr_array_add (other->match_formats, g_strdup ("BR*"));
  g_free (other->arguments);
  other->arguments = g_strdup ("-c 'touch wrong.txt'");
  g_ptr_array_add (req->config->plugins, plugin);
  g_ptr_array_add (req->config->plugins, other);
  req->source = bro_source_new_path (BRO_SOURCE_ISO, iso);
  req->mode = BRO_MODE_MKV;
  req->makemkvcon = g_strdup (exe);
  req->mkvmerge = g_strdup (mkvmerge);
  res = bro_run_job (req);
  if (res->status != BRO_JOB_SUCCEEDED)
    g_printerr ("job failed: %s\n", res->error);
  g_assert_cmpint (res->status, ==, BRO_JOB_SUCCEEDED);
  {
    g_autofree char *folder = g_path_get_basename (res->output_dir);
    g_assert_cmpstr (folder, ==, "One Piece - Season 2 Part 7 Disc 2");
  }
  g_assert_cmpuint (res->files->len, ==, 7);
  for (guint i = 0; i < 7; i++)
    {
      g_autofree char *base = g_path_get_basename (res->files->pdata[i]);
      g_autofree char *want = i < 6 ? g_strdup_printf ("One Piece - Episode %u - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch %u-%u - DVD.mkv",
                                                       138 + i, 1 + 7 * i, 7 + 7 * i)
                                    : g_strdup ("One Piece - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 43 - DVD.mkv");
      g_assert_cmpstr (base, ==, want);
    }
  {
    g_autoptr (GPtrArray) bad = bro_checksums_verify (res->output_dir, NULL);
    g_autofree char *record = g_build_filename (res->output_dir, "bromelia.json", NULL);
    g_autofree char *text = NULL;
    g_autofree char *wrong = g_build_filename (res->output_dir, "wrong.txt", NULL);
    plugin_out = g_build_filename (res->output_dir, "plugin.txt", NULL);
    g_assert_nonnull (bad);
    g_assert_cmpuint (bad->len, ==, 0);
    g_assert_true (g_file_test (record, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_get_contents (plugin_out, &text, NULL, NULL));
    g_assert_cmpstr (text, ==, "One Piece|DVD|7\n");
    g_assert_false (g_file_test (wrong, G_FILE_TEST_EXISTS));
  }
  bro_run_result_free (res);
  bro_run_request_free (req);
  {
    const char *argv[] = { "/bin/rm", "-rf", out, NULL };
    bro_process_run (argv, NULL, NULL, 60, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  }
}

/* ---- safeguards: read errors, staging and marked folders, checks against the listing, disc changes ---- */

#define READ_ERROR "MSG:2003,0,3,\"Error 'Scsi error - MEDIUM ERROR:L-EC UNCORRECTABLE ERROR' occurred while reading " \
  "'/BDMV/STREAM/00001.m2ts' at offset '1048576'\",\"Error '%1' occurred while reading '%2' at offset '%3'\"," \
  "\"Scsi error - MEDIUM ERROR:L-EC UNCORRECTABLE ERROR\",\"/BDMV/STREAM/00001.m2ts\",\"1048576\""
#define SAVED "MSG:5036,260,1,\"Copy complete. 1 titles saved.\",\"Copy complete. %1 titles saved.\",\"1\""
#define FAILED_SAVE "MSG:5003,0,2,\"Failed to save title 1 to file title_t01.mkv\",\"Failed to save title %1 to file %2\",\"1\",\"title_t01.mkv\"\n" \
  "MSG:5037,516,2,\"Copy complete. 0 titles saved, 1 failed.\",\"Copy complete. %1 titles saved, %2 failed.\",\"0\",\"1\""

/* A disc listing in robot format; titles is "duration,source;duration,source;…". */
static char *
listing (const char *volume, const char *titles)
{
  g_auto (GStrv) parts = g_strsplit (titles, ";", -1);
  GString *s = g_string_new (NULL);
  g_string_append_printf (s, "MSG:1005,0,1,\"MakeMKV v1.18.1 started\",\"%%1 started\",\"MakeMKV v1.18.1\"\nTCOUNT:%u\n"
                             "CINFO:1,6209,\"Blu-ray disc\"\nCINFO:2,0,\"%s\"\nCINFO:32,0,\"%s\"\n", g_strv_length (parts), volume, volume);
  for (guint i = 0; parts[i]; i++)
    {
      g_auto (GStrv) f = g_strsplit (parts[i], ",", 2);
      g_string_append_printf (s, "TINFO:%u,8,0,\"2\"\nTINFO:%u,9,0,\"%s\"\nTINFO:%u,16,0,\"0000%s.mpls\"\nTINFO:%u,24,0,\"%s\"\n"
                                 "TINFO:%u,26,0,\"%s\"\nTINFO:%u,27,0,\"title_t0%u.mkv\"\nSINFO:%u,0,1,6201,\"Video\"\nSINFO:%u,1,1,6202,\"Audio\"\n",
                              i, i, f[0], i, f[1], i, f[1], i, f[1], i, i, i, i);
    }
  g_string_append (s, "MSG:5011,0,0,\"Operation successfully completed\",\"Operation successfully completed\"\n");
  return g_string_free (s, FALSE);
}

/* Shell code that writes a small file as MakeMKV's output for the title (for "all": titles 0-4 in the listing),
 * then prints messages. __LISTING__ is replaced by the listing file. */
static char *
writes_file (const char *messages)
{
  return g_strdup_printf ("if [ \"$title\" = \"all\" ]; then\n"
                          "  for t in 0 1 2 3 4; do grep -q \"title_t0$t.mkv\" '__LISTING__' && printf 'mkv data' > \"$dest/title_t0$t.mkv\"; done\n"
                          "else\n  printf 'mkv data %%s' \"$title\" > \"$dest/title_t0$title.mkv\"\nfi\n"
                          "cat <<'EOF'\n%s\nEOF", messages);
}

typedef struct {
  char *dir, *exe, *root;
} Fake;

/* A stand-in for makemkvcon: info prints the listing, mkv runs the shell code with $title and $dest set. */
static Fake *
fake_new_full (const char *listing_text, const char *mkv, const char *backup)
{
  Fake *f = g_new0 (Fake, 1);
  g_autofree char *list = NULL, *script = NULL, *body = NULL;
  g_auto (GStrv) pieces = NULL;
  f->dir = g_dir_make_tmp ("bromelia-fake-XXXXXX", NULL);
  f->root = g_dir_make_tmp ("bromelia-safety-XXXXXX", NULL);
  f->exe = g_build_filename (f->dir, "makemkvcon", NULL);
  list = g_build_filename (f->dir, "listing.txt", NULL);
  g_assert_true (g_file_set_contents (list, listing_text, -1, NULL));
  pieces = g_strsplit (mkv, "__LISTING__", -1);
  body = g_strjoinv (list, pieces);
  script = g_strdup_printf ("#!/bin/sh\necho \"$@\" >> '%s/calls.txt'\n"
                            "while [ $# -gt 0 ]; do\n  case \"$1\" in info|mkv|backup) cmd=\"$1\"; shift; break;; esac\n  shift\ndone\n"
                            "case \"$cmd\" in\n  info) cat '%s' ;;\n  mkv) title=\"$2\"; dest=\"$3\"\n%s\n  ;;\n"
                            "  backup) for a in \"$@\"; do dest=\"$a\"; done\n%s\n  ;;\nesac\nexit 0\n",
                            f->dir, list, body, backup ? backup : ":");
  g_assert_true (g_file_set_contents (f->exe, script, -1, NULL));
  g_chmod (f->exe, 0755);
  return f;
}

static Fake *
fake_new (const char *listing_text, const char *mkv)
{
  return fake_new_full (listing_text, mkv, NULL);
}

static char *
fake_calls (Fake *f)
{
  g_autofree char *p = g_build_filename (f->dir, "calls.txt", NULL);
  char *text = NULL;
  return g_file_get_contents (p, &text, NULL, NULL) ? text : g_strdup ("");
}

static void
fake_free (Fake *f)
{
  g_free (f->dir);
  g_free (f->exe);
  g_free (f->root);
  g_free (f);
}

/* Runs a job against the fake. titles is "0,1" (manual titles) or NULL. */
static BroRunResult *
run_fake (Fake *f, const char *titles, BroDiscInfo *opened, const char *mkvmerge, gboolean shared)
{
  BroRunRequest *req = bro_run_request_new ();
  BroRunResult *res;
  req->job_id = g_uuid_string_random ();
  req->job_dir = g_build_filename (f->dir, "job", NULL);
  req->config = bro_app_config_new ();
  g_free (req->config->output_root);
  req->config->output_root = g_strdup (f->root);
  req->drive = bro_drive_config_new ();
  req->drive->automation.notify = FALSE;
  req->drive->rip.titles.skip_duplicates = FALSE;
  if (shared)
    {
      g_free (req->drive->output.folder_template);
      req->drive->output.folder_template = g_strdup ("");
      req->drive->output.conflict_policy = BRO_CONFLICT_OVERWRITE;
    }
  req->source = bro_source_new_path (BRO_SOURCE_ISO, "/nonexistent/test.iso");
  req->mode = BRO_MODE_MKV;
  if (titles)
    {
      g_auto (GStrv) t = g_strsplit (titles, ",", -1);
      req->manual_titles = g_array_new (FALSE, FALSE, sizeof (int));
      for (guint i = 0; t[i]; i++)
        {
          int v = atoi (t[i]);
          g_array_append_val (req->manual_titles, v);
        }
    }
  if (opened)
    req->preloaded = bro_disc_info_ref (opened);
  req->makemkvcon = g_strdup (f->exe);
  req->mkvmerge = g_strdup (mkvmerge);
  req->skip_eject = TRUE;
  res = bro_run_job (req);
  bro_run_request_free (req);
  return res;
}

static GPtrArray *
names_in (const char *dir, gboolean hidden)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *n;
  while (d && (n = g_dir_read_name (d)))
    if (hidden || n[0] != '.')
      g_ptr_array_add (a, g_strdup (n));
  g_ptr_array_sort (a, (GCompareFunc) g_strcmp0);
  return a;
}

static gboolean
has_name (GPtrArray *a, const char *name)
{
  for (guint i = 0; i < a->len; i++)
    if (g_str_equal (a->pdata[i], name))
      return TRUE;
  return FALSE;
}

static gboolean
has_prefix (GPtrArray *a, const char *prefix)
{
  for (guint i = 0; i < a->len; i++)
    if (g_str_has_prefix (a->pdata[i], prefix))
      return TRUE;
  return FALSE;
}

static char *
record_status (const char *dir, guint *read_errors)
{
  g_autofree char *path = g_build_filename (dir, "bromelia.json", NULL);
  g_autoptr (JsonParser) p = json_parser_new ();
  JsonObject *o;
  g_assert_true (json_parser_load_from_file (p, path, NULL));
  o = json_node_get_object (json_parser_get_root (p));
  if (read_errors)
    *read_errors = json_array_get_length (json_object_get_array_member (o, "readErrors"));
  return g_strdup (json_object_get_string_member (o, "status"));
}

static void
test_safety_success (void)
{
  g_autofree char *l = listing ("SAMPLE_MOVIE", "0:00:10,1;0:00:20,2"), *m = writes_file (SAVED);
  Fake *f = fake_new (l, m);
  g_autoptr (BroRunResult) res = run_fake (f, "0,1", NULL, NULL, FALSE);
  g_autoptr (GPtrArray) root = names_in (f->root, FALSE);
  g_autoptr (GPtrArray) items = NULL;
  g_autoptr (GPtrArray) bad = NULL;
  g_autofree char *status = NULL;
  g_assert_cmpint (res->status, ==, BRO_JOB_SUCCEEDED);
  g_assert_cmpuint (root->len, ==, 1);
  g_assert_cmpstr (root->pdata[0], ==, "Sample Movie");
  items = names_in (res->output_dir, TRUE);
  g_assert_false (has_prefix (items, ".bromelia"));
  g_assert_true (has_name (items, "SHA256SUMS"));
  g_assert_cmpuint (res->files->len, ==, 2);
  for (guint i = 0; i < res->files->len; i++)
    {
      g_autofree char *parent = g_path_get_dirname (res->files->pdata[i]);
      g_assert_cmpstr (parent, ==, res->output_dir);
      g_assert_true (g_file_test (res->files->pdata[i], G_FILE_TEST_EXISTS));
    }
  bad = bro_checksums_verify (res->output_dir, NULL);
  g_assert_nonnull (bad);
  g_assert_cmpuint (bad->len, ==, 0);
  status = record_status (res->output_dir, NULL);
  g_assert_cmpstr (status, ==, "success");
  fake_free (f);
}

static void
test_safety_read_errors (void)
{
  g_autofree char *l = listing ("SAMPLE_MOVIE", "0:00:10,1"), *m = writes_file (READ_ERROR "\n" SAVED);
  Fake *f = fake_new (l, m);
  g_autoptr (BroRunResult) res = run_fake (f, "0", NULL, NULL, FALSE);
  g_autoptr (GPtrArray) root = names_in (f->root, FALSE);
  g_autoptr (GPtrArray) items = NULL;
  g_autofree char *status = NULL, *note = NULL, *note_path = NULL;
  guint read_errors = 0;
  g_assert_cmpint (res->status, ==, BRO_JOB_COMPLETED_WITH_ERRORS);
  g_assert_cmpuint (root->len, ==, 1);
  g_assert_cmpstr (root->pdata[0], ==, "Sample Movie [READ ERRORS]");
  items = names_in (res->output_dir, FALSE);
  g_assert_true (has_name (items, "READ ERRORS.txt"));
  g_assert_true (has_name (items, "SHA256SUMS"));
  note_path = g_build_filename (res->output_dir, "READ ERRORS.txt", NULL);
  g_assert_true (g_file_get_contents (note_path, &note, NULL, NULL));
  g_assert_nonnull (strstr (note, "MEDIUM ERROR"));
  status = record_status (res->output_dir, &read_errors);
  g_assert_cmpstr (status, ==, "errors");
  g_assert_cmpuint (read_errors, ==, 1);
  g_assert_cmpstr (bro_job_state_word (res->status), ==, "errors");
  fake_free (f);
}

static void
test_safety_listing_errors (void)
{
  g_autofree char *l0 = listing ("SAMPLE_MOVIE", "0:00:10,1"), *l = g_strconcat (READ_ERROR "\n", l0, NULL), *m = writes_file (SAVED);
  Fake *f = fake_new (l, m);
  g_autoptr (BroRunResult) res = run_fake (f, "0", NULL, NULL, FALSE);
  g_assert_cmpint (res->status, ==, BRO_JOB_SUCCEEDED);
  fake_free (f);
}

static void
test_safety_failed_title (void)
{
  g_autofree char *l = listing ("SAMPLE_MOVIE", "0:00:10,1;0:00:20,2;0:00:30,3");
  const char *m = "printf 'partial' > \"$dest/title_t0$title.mkv\"\n"
                  "if [ \"$title\" = \"1\" ]; then cat <<'EOF'\n" FAILED_SAVE "\nEOF\nelse cat <<'EOF'\n" SAVED "\nEOF\nfi";
  Fake *f = fake_new (l, m);
  g_autoptr (BroRunResult) res = run_fake (f, "0,1", NULL, NULL, FALSE);
  g_autoptr (GPtrArray) root = names_in (f->root, FALSE);
  g_autoptr (GPtrArray) items = NULL;
  g_assert_cmpint (res->status, ==, BRO_JOB_FAILED);
  g_assert_cmpuint (root->len, ==, 1);
  g_assert_cmpstr (root->pdata[0], ==, "Sample Movie [INCOMPLETE]");
  items = names_in (res->output_dir, TRUE);
  g_assert_true (has_name (items, "INCOMPLETE.txt"));
  g_assert_true (has_name (items, "title_t01.mkv"));
  g_assert_false (has_name (items, "SHA256SUMS"));
  g_assert_false (has_prefix (items, ".bromelia"));
  fake_free (f);
}

static void
test_safety_failure_without_files (void)
{
  Fake *f = fake_new (listing ("SAMPLE_MOVIE", "0:00:10,1"), "cat <<'EOF'\n" FAILED_SAVE "\nEOF");
  g_autoptr (BroRunResult) res = run_fake (f, "0", NULL, NULL, FALSE);
  g_autoptr (GPtrArray) root = names_in (f->root, TRUE);
  g_assert_cmpint (res->status, ==, BRO_JOB_FAILED);
  g_assert_cmpuint (root->len, ==, 0);
  g_assert_null (res->output_dir);
  fake_free (f);
}

static void
test_safety_shared_folder (void)
{
  g_autofree char *m = writes_file (FAILED_SAVE);
  Fake *f = fake_new (listing ("SAMPLE_MOVIE", "0:00:10,1"), m);
  g_autofree char *other = g_build_filename (f->root, "other.mkv", NULL);
  g_autoptr (BroRunResult) res = NULL;
  g_autoptr (GPtrArray) root = NULL;
  g_assert_true (g_file_set_contents (other, "keep", -1, NULL));
  res = run_fake (f, "0", NULL, NULL, TRUE);
  root = names_in (f->root, FALSE);
  g_assert_cmpint (res->status, ==, BRO_JOB_FAILED);
  g_assert_cmpuint (root->len, ==, 2);
  g_assert_true (has_name (root, "other.mkv"));
  g_assert_true (has_prefix (root, "INCOMPLETE - "));
  {
    g_autoptr (GPtrArray) sub = names_in (res->output_dir, FALSE);
    g_assert_true (has_name (sub, "title_t00.mkv"));
  }
  fake_free (f);
}

static void
test_safety_different_disc (void)
{
  g_autofree char *m = writes_file (SAVED), *other = listing ("OTHER_DISC", "0:00:10,1");
  Fake *f = fake_new (listing ("SAMPLE_MOVIE", "0:00:10,1"), m);
  g_autoptr (BroDiscInfo) opened = bro_disc_info_from_output (other);
  g_autoptr (BroRunResult) res = run_fake (f, "0", opened, NULL, FALSE);
  g_autofree char *calls = fake_calls (f);
  g_autoptr (GPtrArray) root = names_in (f->root, TRUE);
  g_assert_cmpint (res->status, ==, BRO_JOB_FAILED);
  g_assert_nonnull (strstr (res->error, "not the disc that was opened"));
  g_assert_null (strstr (calls, " mkv "));
  g_assert_cmpuint (root->len, ==, 0);
  fake_free (f);
}

static void
test_safety_title_numbers (void)
{
  g_autofree char *m = writes_file (SAVED), *old = listing ("SAMPLE_MOVIE", "0:00:10,1;0:00:20,2");
  Fake *f = fake_new (listing ("SAMPLE_MOVIE", "0:00:05,9;0:00:10,1;0:00:20,2"), m);
  g_autoptr (BroDiscInfo) opened = bro_disc_info_from_output (old);
  g_autoptr (BroRunResult) res = run_fake (f, "1", opened, NULL, FALSE);
  g_autofree char *calls = fake_calls (f);
  if (res->status != BRO_JOB_SUCCEEDED)
    g_printerr ("job failed: %s\n", res->error);
  g_assert_cmpint (res->status, ==, BRO_JOB_SUCCEEDED);
  g_assert_nonnull (strstr (calls, "mkv iso:/nonexistent/test.iso 2 "));
  g_assert_null (strstr (calls, "mkv iso:/nonexistent/test.iso 1 "));

  {
    g_autofree char *m2 = writes_file (SAVED);
    Fake *f2 = fake_new (listing ("SAMPLE_MOVIE", "0:00:10,1"), m2);
    g_autoptr (BroRunResult) res2 = run_fake (f2, "1", opened, NULL, FALSE);
    g_autofree char *calls2 = fake_calls (f2);
    g_assert_cmpint (res2->status, ==, BRO_JOB_FAILED);
    g_assert_nonnull (strstr (res2->error, "not in the new disc listing"));
    g_assert_null (strstr (calls2, " mkv "));
    fake_free (f2);
  }
  fake_free (f);
}

static char *
make_sample (const char *ffmpeg, int seconds)
{
  g_autofree char *dir = g_dir_make_tmp ("bromelia-sample-XXXXXX", NULL);
  char *path = g_strdup_printf ("%s/sample-%d.mkv", dir, seconds);
  g_autofree char *src = g_strdup_printf ("testsrc=size=64x48:rate=5:duration=%d", seconds);
  g_autofree char *snd = g_strdup_printf ("sine=duration=%d", seconds);
  const char *argv[] = { ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", src, "-f", "lavfi", "-i", snd, "-c:v", "mpeg4", "-c:a", "aac", path, NULL };
  int status = -1;
  g_assert_true (bro_process_run (argv, NULL, NULL, 120, NULL, NULL, NULL, &status, NULL, NULL, NULL));
  g_assert_cmpint (status, ==, 0);
  return path;
}

static void
test_safety_checks_rips (void)
{
  g_autofree char *ffmpeg = bro_find_tool ("", "ffmpeg");
  g_autofree char *mkvmerge = bro_find_tool ("", "mkvmerge");
  g_autofree char *good = NULL, *short_file = NULL, *m = NULL;
  Fake *f;
  if (!ffmpeg || !mkvmerge)
    {
      g_test_skip ("needs ffmpeg and mkvmerge");
      return;
    }
  good = make_sample (ffmpeg, 10);
  short_file = make_sample (ffmpeg, 3);
  m = g_strdup_printf ("if [ \"$title\" = \"0\" ]; then cp '%s' \"$dest/title_t00.mkv\"; else cp '%s' \"$dest/title_t01.mkv\"; fi\n"
                       "cat <<'EOF'\n%s\nEOF", good, short_file, SAVED);
  f = fake_new (listing ("SAMPLE_MOVIE", "0:00:10,1;0:00:20,2;0:00:30,3"), m);
  {
    g_autoptr (BroRunResult) res = run_fake (f, "0,1", NULL, mkvmerge, FALSE);
    g_autoptr (GPtrArray) root = names_in (f->root, FALSE);
    g_autoptr (GPtrArray) items = NULL;
    g_autofree char *log_path = g_build_filename (f->dir, "job", "log.txt", NULL);
    g_autofree char *log = NULL;
    g_assert_cmpint (res->status, ==, BRO_JOB_FAILED);
    g_assert_nonnull (strstr (res->error, "doesn't match the disc listing"));
    g_assert_true (g_file_get_contents (log_path, &log, NULL, NULL));
    g_assert_nonnull (strstr (log, "Checked title_t00.mkv"));
    g_assert_cmpuint (root->len, ==, 1);
    g_assert_cmpstr (root->pdata[0], ==, "Sample Movie [INCOMPLETE]");
    items = names_in (res->output_dir, FALSE);
    g_assert_true (has_name (items, "title_t01.mkv"));
  }
  fake_free (f);
}

static BroTitle *
title_of (BroDiscInfo *info, int i)
{
  return bro_disc_info_title (info, i);
}

static void
test_rip_checks (void)
{
  g_autofree char *l = listing ("X", "2:00:10,1;0:10:00,2");
  g_autoptr (BroDiscInfo) info = bro_disc_info_from_output (l);
  const char *json = "{\"container\":{\"recognized\":true,\"properties\":{\"duration\":7212345000000}},"
                     "\"tracks\":[{\"id\":0,\"type\":\"video\"},{\"id\":1,\"type\":\"audio\"}],\"chapters\":[{\"num_entries\":2}]}";
  g_autoptr (BroMkvProbe) p = bro_mkv_probe_parse (json);
  g_autoptr (BroMkvProbe) unknown = bro_mkv_probe_parse ("{\"container\":{\"recognized\":false}}");
  g_assert_nonnull (p);
  g_assert_null (unknown);
  g_assert_cmpuint (p->track_types->len, ==, 2);
  g_assert_cmpint (p->chapters, ==, 2);
  g_assert_cmpfloat_with_epsilon (p->duration, 7212.345, 0.001);
#define CHECK(dur, expect_problems) do { \
    g_autoptr (GPtrArray) pr = g_ptr_array_new_with_free_func (g_free); \
    g_autoptr (GPtrArray) no = g_ptr_array_new_with_free_func (g_free); \
    p->duration = (dur); \
    bro_rip_check (p, title_of (info, 0), pr, no); \
    g_assert_cmpuint (pr->len > 0, ==, (expect_problems)); \
  } while (0)
  CHECK (7212.4, FALSE);
  CHECK (5400, TRUE);
  CHECK (7240, FALSE); /* within 0.5 % of two hours */
  CHECK (7260, TRUE);
#undef CHECK
  {
    g_autoptr (GPtrArray) pr = g_ptr_array_new_with_free_func (g_free);
    g_autoptr (GPtrArray) no = g_ptr_array_new_with_free_func (g_free);
    p->has_duration = FALSE;
    bro_rip_check (p, title_of (info, 0), pr, no);
    g_assert_cmpuint (pr->len, ==, 1);
  }
}

static void
test_listing_matcher (void)
{
  g_autofree char *a = listing ("DISC", "1:00:00,1;0:20:00,2"), *b = listing ("DISC", "0:01:00,7;1:00:00,1;0:20:00,2");
  g_autofree char *c = listing ("OTHER", "1:00:00,1"), *d = listing ("", "0:45:00,3"), *e = listing ("", "1:00:00,1");
  g_autoptr (BroDiscInfo) old = bro_disc_info_from_output (a);
  g_autoptr (BroDiscInfo) now = bro_disc_info_from_output (b);
  g_autoptr (BroDiscInfo) other = bro_disc_info_from_output (c);
  g_autoptr (BroDiscInfo) unlabelled = bro_disc_info_from_output (d);
  g_autoptr (BroDiscInfo) unlabelled_same = bro_disc_info_from_output (e);
  g_autoptr (GArray) idx = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GHashTable) map = NULL;
  g_autoptr (GError) err = NULL;
  g_autofree char *w1 = bro_listing_different_disc (old, now), *w2 = bro_listing_different_disc (old, other);
  g_autofree char *w3 = bro_listing_different_disc (unlabelled_same, unlabelled);
  int zero = 0, one = 1;
  g_array_append_val (idx, zero);
  g_array_append_val (idx, one);
  map = bro_listing_map (idx, old, now, NULL, &err);
  g_assert_nonnull (map);
  g_assert_cmpint (GPOINTER_TO_INT (g_hash_table_lookup (map, GINT_TO_POINTER (0))), ==, 2);
  g_assert_cmpint (GPOINTER_TO_INT (g_hash_table_lookup (map, GINT_TO_POINTER (1))), ==, 3);
  g_assert_null (w1);
  g_assert_nonnull (w2);
  g_assert_nonnull (w3);
  {
    g_autoptr (GHashTable) m2 = bro_listing_map (idx, old, other, NULL, &err);
    g_assert_null (m2);
    g_assert_nonnull (strstr (err->message, "not in the new disc listing"));
  }
}

static void
test_backup_checks (void)
{
  g_autofree char *dir = g_dir_make_tmp ("bromelia-backup-XXXXXX", NULL);
  g_autofree char *bdmv = g_build_filename (dir, "bd", "BDMV", NULL), *bd = g_build_filename (dir, "bd", NULL);
  g_autofree char *index = g_build_filename (bdmv, "index.bdmv", NULL);
  g_autofree char *vts = g_build_filename (dir, "dvd", "VIDEO_TS", NULL), *dvd = g_build_filename (dir, "dvd", NULL);
  g_autofree char *ifo = g_build_filename (vts, "VIDEO_TS.IFO", NULL);
  g_autofree char *folder_iso = g_build_filename (dir, "folder.iso", NULL), *iso = g_build_filename (dir, "disc.iso", NULL);
  g_autofree char *missing = g_build_filename (dir, "missing", NULL);
  char *image = g_malloc0 (40000);
  g_mkdir_with_parents (bdmv, 0755);
  g_mkdir_with_parents (vts, 0755);
  g_mkdir_with_parents (folder_iso, 0755);
#define PROBLEM(p, i, expect) do { g_autofree char *x = bro_backup_problem (p, i); g_assert_cmpint (x != NULL, ==, expect); } while (0)
  PROBLEM (bd, FALSE, TRUE);
  g_file_set_contents (index, "", 0, NULL);
  PROBLEM (bd, FALSE, FALSE);
  g_file_set_contents (ifo, "", 0, NULL);
  PROBLEM (dvd, FALSE, FALSE);
  PROBLEM (missing, FALSE, TRUE);
  PROBLEM (folder_iso, TRUE, TRUE);
  memcpy (image + 32769, "BEA01", 5);
  g_file_set_contents (iso, image, 40000, NULL);
  PROBLEM (iso, TRUE, FALSE);
  memset (image, 0, 40000);
  g_file_set_contents (iso, image, 40000, NULL);
  PROBLEM (iso, TRUE, TRUE);
#undef PROBLEM
  g_free (image);
}

static void
test_errors_state (void)
{
  BroPostStep *step = bro_post_step_new ();
  g_free (step->executable);
  step->executable = g_strdup ("/bin/true");
  step->run_on = BRO_RUN_SUCCESS;
  g_assert_false (bro_post_step_should_run (step, BRO_JOB_COMPLETED_WITH_ERRORS));
  step->run_on = BRO_RUN_FAILURE;
  g_assert_true (bro_post_step_should_run (step, BRO_JOB_COMPLETED_WITH_ERRORS));
  g_assert_true (bro_job_state_finished (BRO_JOB_COMPLETED_WITH_ERRORS));
  bro_post_step_free (step);
}

/* ---- recorded makemkvcon runs, stalls, free space, ISO backups, presets ---- */

static char *
fixture_path (const char *name)
{
  const char *dir = g_getenv ("BROMELIA_FIXTURES");
  g_autofree char *rel = g_build_filename (dir ? dir : "../../shared/fixtures", name, NULL);
  GFile *f = g_file_new_for_path (rel);
  char *abs = g_file_get_path (f);
  g_object_unref (f);
  return abs;
}

/* Runs a job of any mode against the fake. */
static BroRunResult *
run_job (Fake *f, BroRipMode mode, BroSource *source, const char *titles, gboolean iso_backup)
{
  BroRunRequest *req = bro_run_request_new ();
  BroRunResult *res;
  req->job_id = g_uuid_string_random ();
  req->job_dir = g_build_filename (f->dir, "job", NULL);
  req->config = bro_app_config_new ();
  g_free (req->config->output_root);
  req->config->output_root = g_strdup (f->root);
  req->drive = bro_drive_config_new ();
  req->drive->automation.notify = FALSE;
  req->drive->rip.backup_format = iso_backup ? BRO_BACKUP_ISO : BRO_BACKUP_FOLDER;
  req->source = source;
  req->mode = mode;
  if (titles)
    {
      int v = atoi (titles);
      req->manual_titles = g_array_new (FALSE, FALSE, sizeof (int));
      g_array_append_val (req->manual_titles, v);
    }
  req->makemkvcon = g_strdup (f->exe);
  req->skip_eject = TRUE;
  res = bro_run_job (req);
  bro_run_request_free (req);
  return res;
}

/* shared/fixtures/rip-outcomes.json: how a job must end for each recorded makemkvcon run. The same file is replayed
 * by the macOS and Windows tests, so the three implementations can't drift apart. */
static void
test_recorded_runs (void)
{
  g_autofree char *path = fixture_path ("rip-outcomes.json");
  g_autoptr (JsonParser) p = json_parser_new ();
  JsonArray *cases;
  g_assert_true (json_parser_load_from_file (p, path, NULL));
  cases = json_object_get_array_member (json_node_get_object (json_parser_get_root (p)), "cases");
  g_assert_cmpuint (json_array_get_length (cases), >=, 7);
  for (guint i = 0; i < json_array_get_length (cases); i++)
    {
      JsonObject *c = json_array_get_object_element (cases, i);
      const char *name = json_object_get_string_member (c, "fixture");
      const char *status = json_object_get_string_member (c, "status");
      const char *error = json_object_has_member (c, "error") ? json_object_get_string_member (c, "error") : NULL;
      gboolean kept = json_object_get_boolean_member (c, "keptApart");
      g_autofree char *file = g_strconcat (name, ".txt", NULL);
      g_autofree char *fx = fixture_path (file);
      g_autofree char *l = listing ("SAMPLE_MOVIE", "0:00:10,1;0:00:20,2");
      g_autofree char *mkv = g_strdup_printf ("%scat '%s'\nexit %d",
                                              json_object_get_boolean_member (c, "writesFile") ? "printf 'mkv data' > \"$dest/title_t0$title.mkv\"\n" : "",
                                              fx, (int) json_object_get_int_member (c, "exitCode"));
      Fake *f = fake_new (l, mkv);
      g_autoptr (BroRunResult) res = run_fake (f, "0", NULL, NULL, FALSE);
      g_autoptr (GPtrArray) root = names_in (f->root, FALSE);
      g_autofree char *got = g_ascii_strdown (res->error ? res->error : "", -1);
      g_autofree char *want = g_ascii_strdown (error ? error : "", -1);
      if (g_strcmp0 (bro_job_state_word (res->status), status) != 0 || (error && !strstr (got, want)))
        g_error ("%s: expected %s (%s), got %s: %s", name, status, error ? error : "", bro_job_state_word (res->status), res->error);
      if (g_str_equal (status, "success"))
        {
          g_assert_cmpuint (root->len, ==, 1);
          g_assert_cmpstr (root->pdata[0], ==, "Sample Movie");
        }
      else if (kept)
        {
          g_autofree char *dir = NULL;
          g_autoptr (GPtrArray) inside = NULL;
          gboolean mkv_found = FALSE;
          g_assert_cmpuint (root->len, ==, 1);
          g_assert_true (g_str_has_suffix (root->pdata[0], "]"));
          dir = g_build_filename (f->root, root->pdata[0], NULL);
          inside = names_in (dir, FALSE);
          for (guint k = 0; k < inside->len; k++)
            mkv_found |= g_str_has_suffix (inside->pdata[k], ".mkv");
          g_assert_true (mkv_found);
        }
      else if (root->len)
        g_error ("%s: nothing should be left, found %s", name, (char *) root->pdata[0]);
      fake_free (f);
    }
}

static void
test_stuck_process (void)
{
  const char *argv[] = { "/bin/sh", "-c", "echo started; sleep 60", NULL };
  const char *busy[] = { "/bin/sh", "-c", "for i in 1 2 3 4 5 6; do echo $i; sleep 0.5; done", NULL };
  BroRunOptions opt = { 0, 2, SIGTERM };
  BroRunStatus st = { 0 };
  GString *out = g_string_new (NULL);
  gint64 start = g_get_monotonic_time ();
  g_assert_true (bro_process_run_ex (argv, NULL, NULL, &opt, NULL, collect, out, &st, NULL));
  g_assert_true (st.stalled);
  g_assert_cmpint (st.exit_status, !=, 0);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 15 * G_USEC_PER_SEC);
  g_assert_cmpstr (out->str, ==, "started\n");
  g_string_free (out, TRUE);
  memset (&st, 0, sizeof st);
  g_assert_true (bro_process_run_ex (busy, NULL, NULL, &opt, NULL, NULL, NULL, &st, NULL));
  g_assert_false (st.stalled);
  g_assert_cmpint (st.exit_status, ==, 0);
}

typedef struct {
  GCancellable *cancel;
  BroRunStatus st;
} CancelRun;

static gpointer
cancel_worker (gpointer data)
{
  CancelRun *r = data;
  /* Ignores SIGINT like makemkvcon; exec keeps that for sleep. */
  const char *argv[] = { "/bin/sh", "-c", "trap '' INT; exec sleep 60", NULL };
  BroRunOptions opt = { 0, 0, 0 };
  bro_process_run_ex (argv, NULL, NULL, &opt, r->cancel, NULL, NULL, &r->st, NULL);
  return NULL;
}

static void
test_cancel_without_main_loop (void)
{
  /* A cancelled run must end (SIGKILL 5 s after the ignored SIGINT) even though no main loop is running. */
  CancelRun r = { g_cancellable_new (), { 0 } };
  GThread *t = g_thread_new ("run", cancel_worker, &r);
  gint64 start;
  g_usleep (G_USEC_PER_SEC / 2);
  start = g_get_monotonic_time ();
  g_cancellable_cancel (r.cancel);
  g_thread_join (t);
  g_assert_true (r.st.cancelled);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 12 * G_USEC_PER_SEC);
  g_object_unref (r.cancel);
}

static void
test_free_space (void)
{
  g_autofree char *l0 = listing ("SAMPLE_MOVIE", "0:00:10,1");
  g_autofree char *l = g_strdup_printf ("%sTINFO:0,11,0,\"%" G_GINT64_FORMAT "\"\n", l0, (gint64) 1 << 60);
  g_autofree char *m = writes_file (SAVED);
  Fake *f = fake_new (l, m);
  g_autoptr (BroRunResult) res = run_fake (f, "0", NULL, NULL, FALSE);
  g_autofree char *calls = fake_calls (f);
  g_autoptr (GPtrArray) root = names_in (f->root, FALSE);
  g_assert_cmpint (res->status, ==, BRO_JOB_FAILED);
  g_assert_nonnull (strstr (res->error, "Not enough free space"));
  g_assert_null (strstr (calls, " mkv "));
  g_assert_cmpuint (root->len, ==, 0);
  g_assert_cmpint (bro_disk_required (1000), ==, 1000 + ((gint64) 256 << 20));
  g_assert_cmpint (bro_disk_required ((gint64) 100 << 30), ==, ((gint64) 100 << 30) + ((gint64) 2 << 30));
  g_assert_cmpint (bro_disk_available (f->root), >, 0);
  fake_free (f);
}

#define SAVED_LINE "echo 'MSG:5036,260,1,\"Copy complete. 1 titles saved.\",\"Copy complete. %1 titles saved.\",\"1\"'"

static void
test_iso_backups (void)
{
  g_autofree char *l = listing ("SAMPLE_MOVIE", "0:00:10,1");
  g_autofree char *m = writes_file (SAVED);
  {
    Fake *f = fake_new_full (l, ":", "head -c 40000 /dev/zero > \"$dest\"\nprintf 'BEA01' | dd of=\"$dest\" bs=1 seek=32769 conv=notrunc 2>/dev/null\n" SAVED_LINE);
    g_autoptr (BroRunResult) res = run_job (f, BRO_MODE_BACKUP_DECRYPTED, bro_source_new_drive (0, ""), NULL, TRUE);
    if (res->status != BRO_JOB_SUCCEEDED)
      g_error ("ISO backup: %s", res->error);
    g_assert_true (g_str_has_suffix (res->files->pdata[0], ".iso"));
    fake_free (f);
  }
  {
    Fake *f = fake_new_full (l, m, "mkdir -p \"$dest/BDMV\" && printf x > \"$dest/BDMV/index.bdmv\"\n" SAVED_LINE);
    g_autoptr (BroRunResult) res = run_job (f, BRO_MODE_BACKUP_THEN_MKV, bro_source_new_drive (0, ""), NULL, TRUE);
    g_autofree char *calls = fake_calls (f);
    gboolean folder = FALSE;
    if (res->status != BRO_JOB_SUCCEEDED)
      g_error ("folder instead of ISO: %s", res->error);
    for (guint i = 0; i < res->files->len; i++)
      if (g_file_test (res->files->pdata[i], G_FILE_TEST_IS_DIR))
        {
          folder = TRUE;
          g_assert_false (g_str_has_suffix (res->files->pdata[i], ".iso"));
        }
    g_assert_true (folder);
    g_assert_nonnull (strstr (calls, "mkv file:"));
    fake_free (f);
  }
  {
    Fake *f = fake_new_full (l, ":", "mkdir -p \"$dest/junk\" && printf x > \"$dest/junk/file\"\n" SAVED_LINE);
    g_autoptr (BroRunResult) res = run_job (f, BRO_MODE_BACKUP_DECRYPTED, bro_source_new_drive (0, ""), NULL, TRUE);
    g_assert_cmpint (res->status, ==, BRO_JOB_FAILED);
    g_assert_nonnull (strstr (res->error, "Backup failed the check"));
    fake_free (f);
  }
}

static void
test_archive_everything (void)
{
  BroDriveConfig *d = bro_drive_config_new ();
  g_autoptr (BroAppConfig) empty = NULL;
  g_autoptr (JsonParser) p = json_parser_new ();
  g_free (d->name);
  d->name = g_strdup ("Left");
  d->rip.titles.strategy = BRO_STRATEGY_LONGEST;
  bro_drive_config_apply_archive_everything (d);
  g_assert_cmpstr (d->name, ==, "Left");
  g_assert_cmpint (d->rip.mode, ==, BRO_MODE_BACKUP_THEN_MKV);
  g_assert_true (d->rip.keep_backup_after_mkv);
  g_assert_cmpint (d->rip.titles.strategy, ==, BRO_STRATEGY_ALL);
  g_assert_cmpint (d->profile.mode, ==, BRO_PROFILE_GENERATED);
  g_assert_cmpstr (d->profile.generated.selection_rule, ==, "+sel:all");
  g_assert_true (d->archive.verify_rips && d->rip.write_disc_info_json);
  bro_drive_config_free (d);
  g_assert_true (json_parser_load_from_data (p, "{}", -1, NULL));
  empty = bro_app_config_from_json (json_parser_get_root (p));
  g_assert_cmpint (empty->stall_timeout_minutes, ==, 30);
  g_assert_true (empty->prevent_sleep);
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
  g_test_add_func ("/identity/labels", test_labels);
  g_test_add_func ("/identity/resolve", test_identity);
  g_test_add_func ("/identity/naming", test_naming);
  g_test_add_func ("/identity/plugins", test_plugins);
  g_test_add_func ("/config/upgrade", test_config_upgrade);
  g_test_add_func ("/archive/checksums", test_checksums);
  g_test_add_func ("/dvd/navigation", test_dvd_navigation);
  g_test_add_func ("/dvd/jumps-and-numbers", test_dvd_jumps_and_numbers);
  g_test_add_func ("/dvd/iso", test_dvd_iso);
  g_test_add_func ("/runner/split-integration", test_split_integration);
  g_test_add_func ("/safety/success", test_safety_success);
  g_test_add_func ("/safety/read-errors", test_safety_read_errors);
  g_test_add_func ("/safety/listing-errors", test_safety_listing_errors);
  g_test_add_func ("/safety/failed-title", test_safety_failed_title);
  g_test_add_func ("/safety/failure-without-files", test_safety_failure_without_files);
  g_test_add_func ("/safety/shared-folder", test_safety_shared_folder);
  g_test_add_func ("/safety/different-disc", test_safety_different_disc);
  g_test_add_func ("/safety/title-numbers", test_safety_title_numbers);
  g_test_add_func ("/safety/checks-rips", test_safety_checks_rips);
  g_test_add_func ("/safety/rip-checks", test_rip_checks);
  g_test_add_func ("/safety/listing-matcher", test_listing_matcher);
  g_test_add_func ("/safety/backup-checks", test_backup_checks);
  g_test_add_func ("/safety/errors-state", test_errors_state);
  g_test_add_func ("/reliability/recorded-runs", test_recorded_runs);
  g_test_add_func ("/reliability/stuck-process", test_stuck_process);
  g_test_add_func ("/reliability/cancel-without-main-loop", test_cancel_without_main_loop);
  g_test_add_func ("/reliability/free-space", test_free_space);
  g_test_add_func ("/reliability/iso-backups", test_iso_backups);
  g_test_add_func ("/reliability/archive-everything", test_archive_everything);
  return g_test_run ();
}
