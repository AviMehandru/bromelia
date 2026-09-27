/* bro-config.c — configuration model with lenient JSON (de)serialisation. */
#include "bro-config.h"

#include <gio/gio.h>
#include <string.h>

/* ---- enum tables ---- */

typedef struct { int value; const char *json; const char *label; } EnumEntry;

static const EnumEntry rip_modes[] = {
  { BRO_MODE_MKV, "mkv", "Rip titles to MKV" },
  { BRO_MODE_BACKUP, "backup", "Backup (encrypted, 1:1)" },
  { BRO_MODE_BACKUP_DECRYPTED, "backupDecrypted", "Backup (decrypted)" },
  { BRO_MODE_BACKUP_THEN_MKV, "backupThenMkv", "Decrypted backup, then MKV from backup" },
  { BRO_MODE_INFO_ONLY, "infoOnly", "Scan only (save disc information)" },
  { 0, NULL, NULL } };
static const EnumEntry strategies[] = {
  { BRO_STRATEGY_ALL, "all", "All titles" },
  { BRO_STRATEGY_LONGEST, "longest", "Longest title(s)" },
  { BRO_STRATEGY_INDICES, "indices", "Titles matching an index pattern" },
  { BRO_STRATEGY_MANUAL, "manual", "Choose manually" },
  { 0, NULL, NULL } };
static const EnumEntry index_bases[] = {
  { BRO_INDEX_MAKEMKV, "makemkv", "MakeMKV title number (0-based)" },
  { BRO_INDEX_SOURCE, "source", "Source title ID (playlist / VTS)" },
  { 0, NULL, NULL } };
static const EnumEntry backup_formats[] = {
  { BRO_BACKUP_FOLDER, "folder", "Folder (BDMV / VIDEO_TS)" },
  { BRO_BACKUP_ISO, "iso", "ISO image" },
  { 0, NULL, NULL } };
static const EnumEntry conflicts[] = {
  { BRO_CONFLICT_UNIQUE, "uniqueSuffix", "Add a number (Disc (2))" },
  { BRO_CONFLICT_OVERWRITE, "overwrite", "Reuse the existing folder" },
  { BRO_CONFLICT_SKIP, "skip", "Skip the job" },
  { 0, NULL, NULL } };
static const EnumEntry run_conditions[] = {
  { BRO_RUN_SUCCESS, "success", "When the rip succeeds" },
  { BRO_RUN_FAILURE, "failure", "When the rip fails" },
  { BRO_RUN_ALWAYS, "always", "Always" },
  { 0, NULL, NULL } };
static const EnumEntry profile_modes[] = {
  { BRO_PROFILE_MAKEMKV_DEFAULT, "makemkvDefault", "MakeMKV default profile" },
  { BRO_PROFILE_GENERATED, "generated", "Bromelia profile (edit below)" },
  { BRO_PROFILE_CUSTOM_FILE, "customFile", "Custom profile file (.mmcp.xml)" },
  { 0, NULL, NULL } };
static const EnumEntry lpcm_outputs[] = {
  { BRO_LPCM_COPY, "copy", "Copy as is" },
  { BRO_LPCM_LPCM, "lpcm", "Raw LPCM" },
  { BRO_LPCM_WAVEX, "wavex", "LPCM in WAV container" },
  { BRO_LPCM_FLAC_BEST, "flac-best", "FLAC (best compression)" },
  { BRO_LPCM_FLAC_FAST, "flac-fast", "FLAC (fast compression)" },
  { 0, NULL, NULL } };

static const EnumEntry *
enum_find (const EnumEntry *table, int value)
{
  for (const EnumEntry *e = table; e->json; e++)
    if (e->value == value)
      return e;
  return table;
}

static int
enum_parse (const EnumEntry *table, const char *s, int fallback)
{
  if (!s)
    return fallback;
  for (const EnumEntry *e = table; e->json; e++)
    if (g_ascii_strcasecmp (e->json, s) == 0)
      return e->value;
  return fallback;
}

const char *bro_rip_mode_to_string (BroRipMode m) { return enum_find (rip_modes, m)->json; }
const char *bro_rip_mode_label (BroRipMode m) { return enum_find (rip_modes, m)->label; }
const char *bro_strategy_label (BroStrategy s) { return enum_find (strategies, s)->label; }
const char *bro_run_condition_label (BroRunCondition r) { return enum_find (run_conditions, r)->label; }
const char *bro_lpcm_to_string (BroLpcmOutput o) { return enum_find (lpcm_outputs, o)->json; }
const char *bro_lpcm_label (BroLpcmOutput o) { return enum_find (lpcm_outputs, o)->label; }
const char *bro_profile_mode_label (BroProfileMode m) { return enum_find (profile_modes, m)->label; }
const char *bro_conflict_label (BroConflictPolicy p) { return enum_find (conflicts, p)->label; }

const char *
bro_rip_mode_short (BroRipMode m)
{
  switch (m)
    {
    case BRO_MODE_MKV: return "MKV";
    case BRO_MODE_BACKUP: return "Backup";
    case BRO_MODE_BACKUP_DECRYPTED: return "Decrypted backup";
    case BRO_MODE_BACKUP_THEN_MKV: return "Backup + MKV";
    default: return "Scan";
    }
}

gboolean bro_rip_mode_makes_mkv (BroRipMode m) { return m == BRO_MODE_MKV || m == BRO_MODE_BACKUP_THEN_MKV; }
gboolean bro_rip_mode_makes_backup (BroRipMode m) { return m == BRO_MODE_BACKUP || m == BRO_MODE_BACKUP_DECRYPTED || m == BRO_MODE_BACKUP_THEN_MKV; }

/* ---- JSON helpers ---- */

static const char *
get_str (JsonObject *o, const char *k, const char *fallback)
{
  JsonNode *n = o ? json_object_get_member (o, k) : NULL;
  if (n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING)
    return json_node_get_string (n);
  return fallback;
}

static char *
dup_str (JsonObject *o, const char *k, const char *fallback)
{
  return g_strdup (get_str (o, k, fallback));
}

static int
get_int (JsonObject *o, const char *k, int fallback)
{
  JsonNode *n = o ? json_object_get_member (o, k) : NULL;
  if (n && JSON_NODE_HOLDS_VALUE (n))
    {
      GType t = json_node_get_value_type (n);
      if (t == G_TYPE_INT64)
        return (int) json_node_get_int (n);
      if (t == G_TYPE_DOUBLE)
        return (int) json_node_get_double (n);
    }
  return fallback;
}

static gboolean
get_bool (JsonObject *o, const char *k, gboolean fallback)
{
  JsonNode *n = o ? json_object_get_member (o, k) : NULL;
  if (n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_BOOLEAN)
    return json_node_get_boolean (n);
  return fallback;
}

static JsonObject *
get_obj (JsonObject *o, const char *k)
{
  JsonNode *n = o ? json_object_get_member (o, k) : NULL;
  return (n && JSON_NODE_HOLDS_OBJECT (n)) ? json_node_get_object (n) : NULL;
}

static JsonArray *
get_arr (JsonObject *o, const char *k)
{
  JsonNode *n = o ? json_object_get_member (o, k) : NULL;
  return (n && JSON_NODE_HOLDS_ARRAY (n)) ? json_node_get_array (n) : NULL;
}

static GHashTable *
str_table_new (void)
{
  return g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

static GHashTable *
get_str_table (JsonObject *o, const char *k)
{
  GHashTable *t = str_table_new ();
  JsonObject *m = get_obj (o, k);
  if (m)
    {
      g_autoptr (GList) members = json_object_get_members (m);
      for (GList *l = members; l; l = l->next)
        {
          const char *v = get_str (m, l->data, NULL);
          if (v)
            g_hash_table_insert (t, g_strdup (l->data), g_strdup (v));
        }
    }
  return t;
}

static void
add_str_table (JsonBuilder *b, const char *name, GHashTable *t)
{
  g_autoptr (GList) keys = g_hash_table_get_keys (t);
  keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);
  json_builder_set_member_name (b, name);
  json_builder_begin_object (b);
  for (GList *l = keys; l; l = l->next)
    {
      json_builder_set_member_name (b, l->data);
      json_builder_add_string_value (b, g_hash_table_lookup (t, l->data));
    }
  json_builder_end_object (b);
}

#define S(name, value) do { json_builder_set_member_name (b, name); json_builder_add_string_value (b, (value) ? (value) : ""); } while (0)
#define I(name, value) do { json_builder_set_member_name (b, name); json_builder_add_int_value (b, value); } while (0)
#define B(name, value) do { json_builder_set_member_name (b, name); json_builder_add_boolean_value (b, value); } while (0)

/* ---- post steps ---- */

BroPostStep *
bro_post_step_new (void)
{
  BroPostStep *s = g_new0 (BroPostStep, 1);
  s->id = g_uuid_string_random ();
  s->name = g_strdup ("Post-processing script");
  s->enabled = TRUE;
  s->executable = g_strdup ("");
  s->interpreter = g_strdup ("");
  s->arguments = g_strdup ("{outputDir}");
  s->working_directory = g_strdup ("");
  s->run_on = BRO_RUN_SUCCESS;
  s->environment = str_table_new ();
  return s;
}

void
bro_post_step_free (BroPostStep *s)
{
  if (!s)
    return;
  g_free (s->id);
  g_free (s->name);
  g_free (s->executable);
  g_free (s->interpreter);
  g_free (s->arguments);
  g_free (s->working_directory);
  g_hash_table_unref (s->environment);
  g_free (s);
}

static void
post_step_to_json (JsonBuilder *b, const BroPostStep *s)
{
  json_builder_begin_object (b);
  S ("id", s->id);
  S ("name", s->name);
  B ("enabled", s->enabled);
  S ("executable", s->executable);
  S ("interpreter", s->interpreter);
  S ("arguments", s->arguments);
  S ("workingDirectory", s->working_directory);
  S ("runOn", enum_find (run_conditions, s->run_on)->json);
  B ("perFile", s->per_file);
  I ("timeoutSeconds", s->timeout_seconds);
  add_str_table (b, "environment", s->environment);
  B ("failJobOnError", s->fail_job_on_error);
  json_builder_end_object (b);
}

static BroPostStep *
post_step_from_json (JsonObject *o)
{
  BroPostStep *s = g_new0 (BroPostStep, 1);
  const char *id = get_str (o, "id", NULL);
  s->id = id && *id ? g_strdup (id) : g_uuid_string_random ();
  s->name = dup_str (o, "name", "Post-processing script");
  s->enabled = get_bool (o, "enabled", TRUE);
  s->executable = dup_str (o, "executable", "");
  s->interpreter = dup_str (o, "interpreter", "");
  s->arguments = dup_str (o, "arguments", "{outputDir}");
  s->working_directory = dup_str (o, "workingDirectory", "");
  s->run_on = enum_parse (run_conditions, get_str (o, "runOn", NULL), BRO_RUN_SUCCESS);
  s->per_file = get_bool (o, "perFile", FALSE);
  s->timeout_seconds = get_int (o, "timeoutSeconds", 0);
  s->environment = get_str_table (o, "environment");
  s->fail_job_on_error = get_bool (o, "failJobOnError", FALSE);
  return s;
}

BroPostStep *
bro_post_step_copy (const BroPostStep *s)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) n = NULL;
  post_step_to_json (b, s);
  n = json_builder_get_root (b);
  return post_step_from_json (json_node_get_object (n));
}

/* ---- drive config ---- */

static void
drive_config_init_defaults (BroDriveConfig *c)
{
  c->id = g_uuid_string_random ();
  c->name = g_strdup ("Drive");
  c->enabled = TRUE;
  c->match_drive_name = g_strdup ("");
  c->match_device = g_strdup ("");
  c->settings = str_table_new ();
  c->profile.mode = BRO_PROFILE_MAKEMKV_DEFAULT;
  c->profile.custom_path = g_strdup ("");
  c->profile.generated.name = g_strdup ("Bromelia");
  c->profile.generated.selection_rule = g_strdup ("");
  c->profile.generated.set_first_audio_default = TRUE;
  c->profile.generated.set_first_subtitle_default = TRUE;
  c->profile.generated.set_first_forced_subtitle_default = TRUE;
  c->profile.generated.ignore_forced_subtitles_flag = TRUE;
  c->profile.generated.insert_first_chapter00 = TRUE;
  c->profile.generated.lpcm_stereo = BRO_LPCM_LPCM;
  c->profile.generated.lpcm_multichannel = BRO_LPCM_FLAC_BEST;
  c->rip.mode = BRO_MODE_MKV;
  c->rip.keep_backup_after_mkv = TRUE;
  c->rip.titles.longest_count = 1;
  c->rip.titles.index_pattern = g_strdup ("");
  c->rip.titles.include_pattern = g_strdup ("");
  c->rip.titles.exclude_pattern = g_strdup ("");
  c->rip.titles.skip_duplicates = TRUE;
  c->rip.min_length_seconds = -1;
  c->rip.cache_mb = -1;
  c->rip.direct_io = -1;
  c->rip.extra_arguments = g_strdup ("");
  c->output.root_override = g_strdup ("");
  c->output.folder_template = g_strdup ("{disc}");
  c->output.file_name_template = g_strdup ("");
  c->output.backup_subfolder = g_strdup ("backup");
  c->automation.auto_rip_delay_seconds = 10;
  c->automation.eject_when_done = TRUE;
  c->automation.notify = TRUE;
  c->automation.play_sound = TRUE;
  c->post_process = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_post_step_free);
}

BroDriveConfig *
bro_drive_config_new (void)
{
  BroDriveConfig *c = g_new0 (BroDriveConfig, 1);
  drive_config_init_defaults (c);
  return c;
}

void
bro_drive_config_free (BroDriveConfig *c)
{
  if (!c)
    return;
  g_free (c->id);
  g_free (c->name);
  g_free (c->match_drive_name);
  g_free (c->match_device);
  g_hash_table_unref (c->settings);
  g_free (c->profile.custom_path);
  g_free (c->profile.generated.name);
  g_free (c->profile.generated.selection_rule);
  g_free (c->rip.titles.index_pattern);
  g_free (c->rip.titles.include_pattern);
  g_free (c->rip.titles.exclude_pattern);
  g_free (c->rip.extra_arguments);
  g_free (c->output.root_override);
  g_free (c->output.folder_template);
  g_free (c->output.file_name_template);
  g_free (c->output.backup_subfolder);
  g_ptr_array_unref (c->post_process);
  g_free (c);
}

static void
drive_config_build (JsonBuilder *b, const BroDriveConfig *c)
{
  const BroTitleSelection *t = &c->rip.titles;
  const BroGeneratedProfile *g = &c->profile.generated;

  json_builder_begin_object (b);
  S ("id", c->id);
  S ("name", c->name);
  B ("enabled", c->enabled);

  json_builder_set_member_name (b, "match");
  json_builder_begin_object (b);
  S ("driveName", c->match_drive_name);
  S ("devicePath", c->match_device);
  json_builder_end_object (b);

  add_str_table (b, "settings", c->settings);

  json_builder_set_member_name (b, "profile");
  json_builder_begin_object (b);
  S ("mode", enum_find (profile_modes, c->profile.mode)->json);
  S ("customPath", c->profile.custom_path);
  json_builder_set_member_name (b, "generated");
  json_builder_begin_object (b);
  S ("name", g->name);
  S ("selectionRule", g->selection_rule);
  B ("setFirstAudioTrackAsDefault", g->set_first_audio_default);
  B ("setFirstSubtitleTrackAsDefault", g->set_first_subtitle_default);
  B ("setFirstForcedSubtitleTrackAsDefault", g->set_first_forced_subtitle_default);
  B ("ignoreForcedSubtitlesFlag", g->ignore_forced_subtitles_flag);
  B ("useISO639Type2T", g->use_iso639_2t);
  B ("insertFirstChapter00IfMissing", g->insert_first_chapter00);
  S ("lpcmStereo", bro_lpcm_to_string (g->lpcm_stereo));
  S ("lpcmMultichannel", bro_lpcm_to_string (g->lpcm_multichannel));
  json_builder_end_object (b);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "rip");
  json_builder_begin_object (b);
  S ("mode", bro_rip_mode_to_string (c->rip.mode));
  S ("backupFormat", enum_find (backup_formats, c->rip.backup_format)->json);
  B ("keepBackupAfterMKV", c->rip.keep_backup_after_mkv);
  json_builder_set_member_name (b, "titleSelection");
  json_builder_begin_object (b);
  S ("strategy", enum_find (strategies, t->strategy)->json);
  I ("longestCount", t->longest_count);
  S ("indexPattern", t->index_pattern);
  S ("indexBase", enum_find (index_bases, t->index_base)->json);
  I ("minDurationSeconds", t->min_duration_seconds);
  I ("maxDurationSeconds", t->max_duration_seconds);
  I ("minChapters", t->min_chapters);
  I ("maxChapters", t->max_chapters);
  I ("minSizeMB", t->min_size_mb);
  I ("maxSizeMB", t->max_size_mb);
  S ("includePattern", t->include_pattern);
  S ("excludePattern", t->exclude_pattern);
  B ("skipDuplicates", t->skip_duplicates);
  B ("skipAlternateAngles", t->skip_alternate_angles);
  I ("maxTitles", t->max_titles);
  json_builder_end_object (b);
  if (c->rip.min_length_seconds >= 0) I ("minLengthSeconds", c->rip.min_length_seconds);
  if (c->rip.cache_mb >= 0) I ("cacheMB", c->rip.cache_mb);
  if (c->rip.direct_io >= 0) B ("directIO", c->rip.direct_io == 1);
  S ("extraArguments", c->rip.extra_arguments);
  B ("writeDiscInfoJSON", c->rip.write_disc_info_json);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "output");
  json_builder_begin_object (b);
  S ("rootOverride", c->output.root_override);
  S ("folderTemplate", c->output.folder_template);
  S ("fileNameTemplate", c->output.file_name_template);
  S ("backupSubfolder", c->output.backup_subfolder);
  S ("conflictPolicy", enum_find (conflicts, c->output.conflict_policy)->json);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "automation");
  json_builder_begin_object (b);
  B ("autoRipOnInsert", c->automation.auto_rip_on_insert);
  I ("autoRipDelaySeconds", c->automation.auto_rip_delay_seconds);
  B ("ejectWhenDone", c->automation.eject_when_done);
  B ("ejectOnFailure", c->automation.eject_on_failure);
  B ("notify", c->automation.notify);
  B ("playSound", c->automation.play_sound);
  json_builder_end_object (b);

  json_builder_set_member_name (b, "postProcess");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->post_process->len; i++)
    post_step_to_json (b, c->post_process->pdata[i]);
  json_builder_end_array (b);
  json_builder_end_object (b);
}

JsonNode *
bro_drive_config_to_json (const BroDriveConfig *c)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  drive_config_build (b, c);
  return json_builder_get_root (b);
}

static BroDriveConfig *
drive_config_from_object (JsonObject *o)
{
  BroDriveConfig *c = bro_drive_config_new ();
  JsonObject *m, *p, *g, *r, *t, *out, *a;
  JsonArray *steps;
  const char *id = get_str (o, "id", NULL);

  if (id && *id)
    {
      g_free (c->id);
      c->id = g_strdup (id);
    }
#define REPLACE(field, value) do { char *_v = (value); g_free (field); field = _v; } while (0)
  REPLACE (c->name, dup_str (o, "name", "Drive"));
  c->enabled = get_bool (o, "enabled", TRUE);
  m = get_obj (o, "match");
  REPLACE (c->match_drive_name, dup_str (m, "driveName", ""));
  REPLACE (c->match_device, dup_str (m, "devicePath", ""));
  g_hash_table_unref (c->settings);
  c->settings = get_str_table (o, "settings");

  p = get_obj (o, "profile");
  c->profile.mode = enum_parse (profile_modes, get_str (p, "mode", NULL), BRO_PROFILE_MAKEMKV_DEFAULT);
  REPLACE (c->profile.custom_path, dup_str (p, "customPath", ""));
  g = get_obj (p, "generated");
  REPLACE (c->profile.generated.name, dup_str (g, "name", "Bromelia"));
  REPLACE (c->profile.generated.selection_rule, dup_str (g, "selectionRule", ""));
  c->profile.generated.set_first_audio_default = get_bool (g, "setFirstAudioTrackAsDefault", TRUE);
  c->profile.generated.set_first_subtitle_default = get_bool (g, "setFirstSubtitleTrackAsDefault", TRUE);
  c->profile.generated.set_first_forced_subtitle_default = get_bool (g, "setFirstForcedSubtitleTrackAsDefault", TRUE);
  c->profile.generated.ignore_forced_subtitles_flag = get_bool (g, "ignoreForcedSubtitlesFlag", TRUE);
  c->profile.generated.use_iso639_2t = get_bool (g, "useISO639Type2T", FALSE);
  c->profile.generated.insert_first_chapter00 = get_bool (g, "insertFirstChapter00IfMissing", TRUE);
  c->profile.generated.lpcm_stereo = enum_parse (lpcm_outputs, get_str (g, "lpcmStereo", NULL), BRO_LPCM_LPCM);
  c->profile.generated.lpcm_multichannel = enum_parse (lpcm_outputs, get_str (g, "lpcmMultichannel", NULL), BRO_LPCM_FLAC_BEST);

  r = get_obj (o, "rip");
  c->rip.mode = enum_parse (rip_modes, get_str (r, "mode", NULL), BRO_MODE_MKV);
  c->rip.backup_format = enum_parse (backup_formats, get_str (r, "backupFormat", NULL), BRO_BACKUP_FOLDER);
  c->rip.keep_backup_after_mkv = get_bool (r, "keepBackupAfterMKV", TRUE);
  t = get_obj (r, "titleSelection");
  c->rip.titles.strategy = enum_parse (strategies, get_str (t, "strategy", NULL), BRO_STRATEGY_ALL);
  c->rip.titles.longest_count = get_int (t, "longestCount", 1);
  REPLACE (c->rip.titles.index_pattern, dup_str (t, "indexPattern", ""));
  c->rip.titles.index_base = enum_parse (index_bases, get_str (t, "indexBase", NULL), BRO_INDEX_MAKEMKV);
  c->rip.titles.min_duration_seconds = get_int (t, "minDurationSeconds", 0);
  c->rip.titles.max_duration_seconds = get_int (t, "maxDurationSeconds", 0);
  c->rip.titles.min_chapters = get_int (t, "minChapters", 0);
  c->rip.titles.max_chapters = get_int (t, "maxChapters", 0);
  c->rip.titles.min_size_mb = get_int (t, "minSizeMB", 0);
  c->rip.titles.max_size_mb = get_int (t, "maxSizeMB", 0);
  REPLACE (c->rip.titles.include_pattern, dup_str (t, "includePattern", ""));
  REPLACE (c->rip.titles.exclude_pattern, dup_str (t, "excludePattern", ""));
  c->rip.titles.skip_duplicates = get_bool (t, "skipDuplicates", TRUE);
  c->rip.titles.skip_alternate_angles = get_bool (t, "skipAlternateAngles", FALSE);
  c->rip.titles.max_titles = get_int (t, "maxTitles", 0);
  c->rip.min_length_seconds = get_int (r, "minLengthSeconds", -1);
  c->rip.cache_mb = get_int (r, "cacheMB", -1);
  if (r && json_object_has_member (r, "directIO") && JSON_NODE_HOLDS_VALUE (json_object_get_member (r, "directIO")))
    c->rip.direct_io = get_bool (r, "directIO", FALSE) ? 1 : 0;
  REPLACE (c->rip.extra_arguments, dup_str (r, "extraArguments", ""));
  c->rip.write_disc_info_json = get_bool (r, "writeDiscInfoJSON", FALSE);

  out = get_obj (o, "output");
  REPLACE (c->output.root_override, dup_str (out, "rootOverride", ""));
  REPLACE (c->output.folder_template, dup_str (out, "folderTemplate", "{disc}"));
  REPLACE (c->output.file_name_template, dup_str (out, "fileNameTemplate", ""));
  REPLACE (c->output.backup_subfolder, dup_str (out, "backupSubfolder", "backup"));
  c->output.conflict_policy = enum_parse (conflicts, get_str (out, "conflictPolicy", NULL), BRO_CONFLICT_UNIQUE);

  a = get_obj (o, "automation");
  c->automation.auto_rip_on_insert = get_bool (a, "autoRipOnInsert", FALSE);
  c->automation.auto_rip_delay_seconds = get_int (a, "autoRipDelaySeconds", 10);
  c->automation.eject_when_done = get_bool (a, "ejectWhenDone", TRUE);
  c->automation.eject_on_failure = get_bool (a, "ejectOnFailure", FALSE);
  c->automation.notify = get_bool (a, "notify", TRUE);
  c->automation.play_sound = get_bool (a, "playSound", TRUE);

  steps = get_arr (o, "postProcess");
  if (steps)
    for (guint i = 0; i < json_array_get_length (steps); i++)
      {
        JsonNode *n = json_array_get_element (steps, i);
        if (JSON_NODE_HOLDS_OBJECT (n))
          g_ptr_array_add (c->post_process, post_step_from_json (json_node_get_object (n)));
      }
#undef REPLACE
  return c;
}

BroDriveConfig *
bro_drive_config_from_json (JsonNode *node)
{
  return drive_config_from_object (node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL);
}

BroDriveConfig *
bro_drive_config_copy (const BroDriveConfig *c)
{
  g_autoptr (JsonNode) n = bro_drive_config_to_json (c);
  return bro_drive_config_from_json (n);
}

BroDriveConfig *
bro_drive_config_apply_body (const BroDriveConfig *identity, const BroDriveConfig *body)
{
  BroDriveConfig *c = bro_drive_config_copy (body);
  g_free (c->id);
  c->id = g_strdup (identity->id);
  g_free (c->name);
  c->name = g_strdup (identity->name);
  g_free (c->match_drive_name);
  c->match_drive_name = g_strdup (identity->match_drive_name);
  g_free (c->match_device);
  c->match_device = g_strdup (identity->match_device);
  c->enabled = identity->enabled;
  for (guint i = 0; i < c->post_process->len; i++)
    {
      BroPostStep *s = c->post_process->pdata[i];
      g_free (s->id);
      s->id = g_uuid_string_random ();
    }
  return c;
}

char *
bro_normalize_drive_name (const char *s)
{
  g_auto (GStrv) parts = g_strsplit_set (s ? s : "", " \t", -1);
  GString *out = g_string_new (NULL);
  for (int i = 0; parts[i]; i++)
    if (*parts[i])
      g_string_append_printf (out, "%s%s", out->len ? " " : "", parts[i]);
  char *lower = g_ascii_strdown (out->str, -1);
  g_string_free (out, TRUE);
  return lower;
}

gboolean
bro_drive_config_matches (const BroDriveConfig *c, const BroDriveEntry *e)
{
  g_autofree char *name = g_strstrip (g_strdup (c->match_drive_name ? c->match_drive_name : ""));
  if (*name)
    {
      g_autofree char *a = bro_normalize_drive_name (name);
      g_autofree char *b = bro_normalize_drive_name (e->drive_name);
      return g_str_equal (a, b);
    }
  return c->match_device && *c->match_device && g_strcmp0 (c->match_device, e->device) == 0;
}

/* ---- app config ---- */

void
bro_preset_free (BroPreset *p)
{
  if (!p)
    return;
  g_free (p->id);
  g_free (p->name);
  bro_drive_config_free (p->config);
  g_free (p);
}

BroPreset *
bro_preset_new (const char *name, const BroDriveConfig *config)
{
  BroPreset *p = g_new0 (BroPreset, 1);
  p->id = g_uuid_string_random ();
  p->name = g_strdup (name);
  p->config = bro_drive_config_copy (config);
  g_free (p->config->match_drive_name);
  p->config->match_drive_name = g_strdup ("");
  g_free (p->config->match_device);
  p->config->match_device = g_strdup ("");
  return p;
}

BroAppConfig *
bro_app_config_new (void)
{
  BroAppConfig *c = g_new0 (BroAppConfig, 1);
  c->version = 1;
  c->makemkvcon_path = g_strdup ("");
  c->mkvmerge_path = g_strdup ("");
  c->output_root = g_strdup ("~/Videos/Bromelia");
  c->poll_interval_seconds = 10;
  c->registration_key = g_strdup ("");
  c->global_settings = str_table_new ();
  c->default_drive = bro_drive_config_new ();
  g_free (c->default_drive->name);
  c->default_drive->name = g_strdup ("Default");
  c->drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_config_free);
  c->presets = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_preset_free);
  c->history_limit = 500;
  return c;
}

void
bro_app_config_free (BroAppConfig *c)
{
  if (!c)
    return;
  g_free (c->makemkvcon_path);
  g_free (c->mkvmerge_path);
  g_free (c->output_root);
  g_free (c->registration_key);
  g_hash_table_unref (c->global_settings);
  bro_drive_config_free (c->default_drive);
  g_ptr_array_unref (c->drives);
  g_ptr_array_unref (c->presets);
  g_free (c);
}

JsonNode *
bro_app_config_to_json (const BroAppConfig *c)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  I ("version", c->version);
  S ("makemkvconPath", c->makemkvcon_path);
  S ("mkvmergePath", c->mkvmerge_path);
  S ("outputRoot", c->output_root);
  I ("pollIntervalSeconds", c->poll_interval_seconds);
  B ("pollWhileRipping", c->poll_while_ripping);
  I ("maxConcurrentJobs", c->max_concurrent_jobs);
  S ("registrationKey", c->registration_key);
  add_str_table (b, "globalSettings", c->global_settings);
  json_builder_set_member_name (b, "defaultDrive");
  drive_config_build (b, c->default_drive);
  json_builder_set_member_name (b, "drives");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->drives->len; i++)
    drive_config_build (b, c->drives->pdata[i]);
  json_builder_end_array (b);
  json_builder_set_member_name (b, "presets");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->presets->len; i++)
    {
      BroPreset *p = c->presets->pdata[i];
      json_builder_begin_object (b);
      S ("id", p->id);
      S ("name", p->name);
      json_builder_set_member_name (b, "config");
      drive_config_build (b, p->config);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  I ("historyLimit", c->history_limit);
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

BroAppConfig *
bro_app_config_from_json (JsonNode *node)
{
  BroAppConfig *c = bro_app_config_new ();
  JsonObject *o = node && JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
  JsonArray *arr;
  if (!o)
    return c;
#define REPLACE(field, value) do { char *_v = (value); g_free (field); field = _v; } while (0)
  c->version = get_int (o, "version", 1);
  REPLACE (c->makemkvcon_path, dup_str (o, "makemkvconPath", ""));
  REPLACE (c->mkvmerge_path, dup_str (o, "mkvmergePath", ""));
  REPLACE (c->output_root, dup_str (o, "outputRoot", "~/Videos/Bromelia"));
  c->poll_interval_seconds = get_int (o, "pollIntervalSeconds", 10);
  c->poll_while_ripping = get_bool (o, "pollWhileRipping", FALSE);
  c->max_concurrent_jobs = get_int (o, "maxConcurrentJobs", 0);
  REPLACE (c->registration_key, dup_str (o, "registrationKey", ""));
  g_hash_table_unref (c->global_settings);
  c->global_settings = get_str_table (o, "globalSettings");
  if (get_obj (o, "defaultDrive"))
    {
      bro_drive_config_free (c->default_drive);
      c->default_drive = drive_config_from_object (get_obj (o, "defaultDrive"));
    }
  if ((arr = get_arr (o, "drives")))
    for (guint i = 0; i < json_array_get_length (arr); i++)
      if (JSON_NODE_HOLDS_OBJECT (json_array_get_element (arr, i)))
        g_ptr_array_add (c->drives, drive_config_from_object (json_array_get_object_element (arr, i)));
  if ((arr = get_arr (o, "presets")))
    for (guint i = 0; i < json_array_get_length (arr); i++)
      if (JSON_NODE_HOLDS_OBJECT (json_array_get_element (arr, i)))
        {
          JsonObject *po = json_array_get_object_element (arr, i);
          BroPreset *p = g_new0 (BroPreset, 1);
          const char *id = get_str (po, "id", NULL);
          p->id = id && *id ? g_strdup (id) : g_uuid_string_random ();
          p->name = dup_str (po, "name", "Preset");
          p->config = drive_config_from_object (get_obj (po, "config"));
          g_ptr_array_add (c->presets, p);
        }
  c->history_limit = get_int (o, "historyLimit", 500);
#undef REPLACE
  return c;
}

char *
bro_json_to_string (JsonNode *node, gboolean pretty)
{
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, node);
  json_generator_set_pretty (gen, pretty);
  return json_generator_to_data (gen, NULL);
}

char *
bro_app_config_serialize (const BroAppConfig *c)
{
  g_autoptr (JsonNode) n = bro_app_config_to_json (c);
  return bro_json_to_string (n, TRUE);
}

BroAppConfig *
bro_app_config_parse (const char *json, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, -1, error))
    return NULL;
  return bro_app_config_from_json (json_parser_get_root (parser));
}

BroAppConfig *
bro_app_config_copy (const BroAppConfig *c)
{
  g_autoptr (JsonNode) n = bro_app_config_to_json (c);
  return bro_app_config_from_json (n);
}

BroAppConfig *
bro_app_config_load (const char *path)
{
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;
  BroAppConfig *c;
  if (!g_file_get_contents (path, &text, NULL, NULL))
    return bro_app_config_new ();
  c = bro_app_config_parse (text, &error);
  if (!c)
    {
      /* Keep the unreadable file for the user instead of silently overwriting it. */
      g_autofree char *backup = g_strdup_printf ("%s.broken-%" G_GINT64_FORMAT, path, g_get_real_time () / G_USEC_PER_SEC);
      g_file_set_contents (backup, text, -1, NULL);
      g_warning ("Could not read %s: %s", path, error->message);
      return bro_app_config_new ();
    }
  return c;
}

gboolean
bro_write_file_atomic (const char *path, const char *contents, GError **error)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);
  return g_file_set_contents (path, contents, -1, error);
}

gboolean
bro_app_config_save (const BroAppConfig *c, const char *path, GError **error)
{
  g_autofree char *text = bro_app_config_serialize (c);
  return bro_write_file_atomic (path, text, error);
}

BroDriveConfig *
bro_app_config_drive_for_entry (BroAppConfig *c, const BroDriveEntry *e)
{
  for (guint i = 0; i < c->drives->len; i++)
    {
      BroDriveConfig *d = c->drives->pdata[i];
      if (d->enabled && bro_drive_config_matches (d, e))
        return d;
    }
  for (guint i = 0; i < c->drives->len; i++)
    if (bro_drive_config_matches (c->drives->pdata[i], e))
      return c->drives->pdata[i];
  return NULL;
}

BroDriveConfig *
bro_app_config_drive_by_id (BroAppConfig *c, const char *id)
{
  if (!id)
    return NULL;
  if (g_strcmp0 (c->default_drive->id, id) == 0)
    return c->default_drive;
  for (guint i = 0; i < c->drives->len; i++)
    if (g_strcmp0 (((BroDriveConfig *) c->drives->pdata[i])->id, id) == 0)
      return c->drives->pdata[i];
  return NULL;
}

GHashTable *
bro_app_config_effective_settings (const BroAppConfig *c, const BroDriveConfig *d)
{
  GHashTable *s = str_table_new ();
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, c->global_settings);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_replace (s, g_strdup (k), g_strdup (v));
  g_hash_table_iter_init (&it, d->settings);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_replace (s, g_strdup (k), g_strdup (v));
  if (c->registration_key && *c->registration_key)
    g_hash_table_replace (s, g_strdup ("app_Key"), g_strdup (c->registration_key));
  return s;
}

const char *
bro_app_config_output_root (const BroAppConfig *c, const BroDriveConfig *d)
{
  if (d->output.root_override && *g_strstrip (d->output.root_override))
    return d->output.root_override;
  return c->output_root;
}
