/* bro-runner.c — job pipeline, post-processing and mkvmerge remuxing. */
#include "bro-runner.h"
#include "bro-logic.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

const char *
bro_job_state_label (BroJobState s)
{
  switch (s)
    {
    case BRO_JOB_QUEUED: return "Queued";
    case BRO_JOB_WAITING: return "Starting soon";
    case BRO_JOB_RUNNING: return "Running";
    case BRO_JOB_SUCCEEDED: return "Completed";
    case BRO_JOB_FAILED: return "Failed";
    default: return "Cancelled";
    }
}

const char *
bro_job_state_word (BroJobState s)
{
  return s == BRO_JOB_SUCCEEDED ? "success" : s == BRO_JOB_CANCELLED ? "cancelled" : "failed";
}

gboolean
bro_job_state_finished (BroJobState s)
{
  return s == BRO_JOB_SUCCEEDED || s == BRO_JOB_FAILED || s == BRO_JOB_CANCELLED;
}

GHashTable *
bro_int_set_new (void)
{
  return g_hash_table_new (g_direct_hash, g_direct_equal);
}

GHashTable *
bro_track_selections_new (void)
{
  return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref);
}

BroRunRequest *
bro_run_request_new (void)
{
  BroRunRequest *r = g_new0 (BroRunRequest, 1);
  r->track_selections = bro_track_selections_new ();
  r->name_overrides = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  r->cancellable = g_cancellable_new ();
  return r;
}

void
bro_run_request_free (BroRunRequest *r)
{
  if (!r)
    return;
  g_free (r->job_id);
  g_free (r->job_dir);
  bro_app_config_free (r->config);
  bro_drive_config_free (r->drive);
  bro_source_free (r->source);
  g_free (r->disc_label);
  if (r->preloaded) bro_disc_info_unref (r->preloaded);
  if (r->manual_titles) g_array_unref (r->manual_titles);
  g_hash_table_unref (r->track_selections);
  g_hash_table_unref (r->name_overrides);
  g_free (r->makemkvcon);
  g_free (r->mkvmerge);
  g_clear_object (&r->cancellable);
  g_free (r);
}

void
bro_run_result_free (BroRunResult *r)
{
  if (!r)
    return;
  g_free (r->error);
  g_free (r->output_dir);
  g_ptr_array_unref (r->files);
  g_free (r->disc_label);
  if (r->info) bro_disc_info_unref (r->info);
  g_free (r);
}

char *
bro_unique_path (const char *path)
{
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return g_strdup (path);
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *base = g_path_get_basename (path);
  const char *dot = g_file_test (path, G_FILE_TEST_IS_DIR) ? NULL : strrchr (base, '.');
  g_autofree char *stem = dot ? g_strndup (base, dot - base) : g_strdup (base);
  for (int n = 2;; n++)
    {
      g_autofree char *name = dot ? g_strdup_printf ("%s (%d)%s", stem, n, dot) : g_strdup_printf ("%s (%d)", stem, n);
      char *cand = g_build_filename (dir, name, NULL);
      if (!g_file_test (cand, G_FILE_TEST_EXISTS))
        return cand;
      g_free (cand);
    }
}

/* ---------------------------------------------------------------------------------------------- */

typedef struct {
  int exit_status;
  int saved, failed; /* -1 when not reported */
  GPtrArray *errors;
  BroDiscInfo *info;
  char *drive_mismatch;
  gboolean cancelled;
} Summary;

typedef struct {
  BroRunRequest *req;
  BroRunResult *res;
  FILE *log;
  BroMakemkvEnv *env;      /* current */
  BroMakemkvEnv *base_env;
  BroMakemkvEnv *all_env;  /* for titles with hand-picked tracks */
  gboolean show_debug;
  int step_index, step_count;
  char *last_total;
  int expected_index;
  char *expected_device;
  Summary *sum;
  GArray *rip_titles;
  GPtrArray *commands;
} Ctx;

static void
update (Ctx *c, BroUpdateKind kind, BroSeverity sev, const char *text, double cur, double tot)
{
  BroUpdate u = { kind, sev, (char *) text, cur, tot, c->step_index, c->step_count };
  if (c->req->on_update)
    c->req->on_update (&u, c->req->user_data);
}

static void
log_line (Ctx *c, BroSeverity sev, const char *fmt, ...) G_GNUC_PRINTF (3, 4);

static void
log_line (Ctx *c, BroSeverity sev, const char *fmt, ...)
{
  va_list ap;
  g_autofree char *text = NULL;
  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  if (sev == BRO_SEV_WARNING) c->res->warnings++;
  if (sev == BRO_SEV_ERROR) c->res->errors++;
  if (c->log)
    {
      g_autoptr (GDateTime) now = g_date_time_new_now_local ();
      g_autofree char *stamp = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
      if (sev == BRO_SEV_INFO)
        fprintf (c->log, "%s %s\n", stamp, text);
      else
        fprintf (c->log, "%s [%s] %s\n", stamp, bro_severity_name (sev), text);
      fflush (c->log);
    }
  update (c, BRO_UPDATE_LOG, sev, text, 0, 0);
}

static void
phase (Ctx *c, const char *text)
{
  update (c, BRO_UPDATE_PHASE, BRO_SEV_INFO, text, 0, 0);
}

static void
steps (Ctx *c, int index, int count)
{
  c->step_index = index;
  c->step_count = count;
  update (c, BRO_UPDATE_STEPS, BRO_SEV_INFO, NULL, 0, 0);
}

static gboolean
cancelled (Ctx *c)
{
  return g_cancellable_is_cancelled (c->req->cancellable);
}

static void
summary_free (Summary *s)
{
  if (!s) return;
  g_ptr_array_unref (s->errors);
  if (s->info) bro_disc_info_unref (s->info);
  g_free (s->drive_mismatch);
  g_free (s);
}

static void
on_makemkv_line (const char *line, gpointer data)
{
  Ctx *c = data;
  Summary *s = c->sum;
  g_autoptr (BroEvent) ev = bro_event_parse (line);
  if (!ev)
    return;
  bro_disc_info_consume (s->info, ev);
  switch (ev->type)
    {
    case BRO_EV_MESSAGE:
      {
        BroSeverity sev = bro_event_severity (ev);
        if (sev == BRO_SEV_DEBUG && !c->show_debug)
          return;
        log_line (c, sev, "%s", ev->text);
        if (sev == BRO_SEV_ERROR)
          g_ptr_array_add (s->errors, g_strdup (ev->text));
        if ((ev->code == 5036 || ev->code == 5005) && ev->params->len > 0)
          s->saved = atoi (ev->params->pdata[0]);
        if (ev->code == 5037 && ev->params->len > 1)
          {
            s->saved = atoi (ev->params->pdata[0]);
            s->failed = atoi (ev->params->pdata[1]);
          }
      }
      break;
    case BRO_EV_PROGRESS_TOTAL:
      update (c, BRO_UPDATE_TOTAL_OPERATION, BRO_SEV_INFO, ev->text, 0, 0);
      if (g_strcmp0 (ev->text, c->last_total) != 0)
        {
          g_free (c->last_total);
          c->last_total = g_strdup (ev->text);
          log_line (c, BRO_SEV_INFO, "— %s", ev->text);
        }
      break;
    case BRO_EV_PROGRESS_CURRENT:
      update (c, BRO_UPDATE_OPERATION, BRO_SEV_INFO, ev->text, 0, 0);
      break;
    case BRO_EV_PROGRESS_VALUE:
      if (ev->max > 0)
        update (c, BRO_UPDATE_PROGRESS, BRO_SEV_INFO, NULL, (double) ev->current / ev->max, (double) ev->total / ev->max);
      break;
    case BRO_EV_DRIVE:
      if (c->expected_device && *c->expected_device && ev->drive.index == c->expected_index &&
          ev->drive.device && *ev->drive.device && g_strcmp0 (ev->drive.device, c->expected_device) != 0 && !s->drive_mismatch)
        {
          s->drive_mismatch = g_strdup_printf ("MakeMKV drive %d is now %s, expected %s. The job was stopped to avoid reading "
                                               "the wrong disc; rescan drives and retry.", c->expected_index, ev->drive.device,
                                               c->expected_device);
          g_cancellable_cancel (c->req->cancellable);
        }
      break;
    case BRO_EV_RAW:
      log_line (c, BRO_SEV_INFO, "%s", ev->text);
      break;
    default:
      break;
    }
}

/* Runs makemkvcon with the current environment. Returns NULL (with error) when it could not run or was cancelled. */
static Summary *
run_makemkv (Ctx *c, GPtrArray *args, GError **error)
{
  Summary *s = g_new0 (Summary, 1);
  g_auto (GStrv) envp = bro_makemkv_env_environ (c->env);
  g_autofree char *cmd = NULL;
  gboolean was_cancelled = FALSE;

  s->saved = s->failed = -1;
  s->errors = g_ptr_array_new_with_free_func (g_free);
  s->info = bro_disc_info_new ();
  g_ptr_array_add (args, NULL);
  cmd = bro_command_line ((const char *const *) args->pdata);
  g_ptr_array_add (c->commands, g_strdup (cmd));
  update (c, BRO_UPDATE_COMMAND, BRO_SEV_INFO, cmd, 0, 0);
  log_line (c, BRO_SEV_INFO, "$ %s", cmd);
  g_free (c->last_total);
  c->last_total = NULL;

  c->sum = s;
  if (!bro_process_run ((const char *const *) args->pdata, (const char *const *) envp, c->env->home, 0, c->req->cancellable,
                        on_makemkv_line, c, &s->exit_status, &was_cancelled, NULL, error))
    {
      c->sum = NULL;
      summary_free (s);
      if (error && *error && !g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_prefix_error (error, "Could not start makemkvcon: ");
      return NULL;
    }
  c->sum = NULL;
  if (s->drive_mismatch)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, s->drive_mismatch);
      summary_free (s);
      return NULL;
    }
  if (was_cancelled || cancelled (c))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      summary_free (s);
      return NULL;
    }
  return s;
}

static BroDiscInfo *
scan_disc (Ctx *c, const BroSource *src, GError **error)
{
  g_autoptr (GPtrArray) args = bro_makemkv_info_args (c->env, src, &c->req->drive->rip);
  Summary *s;
  BroDiscInfo *info;
  phase (c, "Reading disc information");
  s = run_makemkv (c, args, error);
  if (!s)
    return NULL;
  if (s->info->titles->len == 0)
    {
      g_autofree char *name = bro_source_display_name (src);
      const char *reason = s->errors->len ? g_ptr_array_index (s->errors, s->errors->len - 1) : NULL;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read titles from %s: %s", name,
                   reason ? reason : "makemkvcon reported no titles");
      summary_free (s);
      return NULL;
    }
  info = bro_disc_info_ref (s->info);
  log_line (c, BRO_SEV_INFO, "Found %u title(s) on “%s”", info->titles->len, bro_disc_info_name (info));
  summary_free (s);
  return info;
}

static GHashTable *
base_values (Ctx *c, BroJobState status)
{
  GHashTable *v = bro_template_values_new ();
  BroDiscInfo *info = c->res->info;
  const char *disc = info && *bro_disc_info_name (info) ? bro_disc_info_name (info)
                     : (c->res->disc_label && *c->res->disc_label ? c->res->disc_label : "Disc");
  g_autofree char *job = g_strndup (c->req->job_id, 8);
  bro_template_values_set (v, "disc", disc);
  bro_template_values_set (v, "volume", info && bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME) ? bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME) : c->res->disc_label);
  bro_template_values_set (v, "type", bro_disc_info_type_token (info));
  bro_template_values_set (v, "drive", c->req->drive->name);
  bro_template_values_set (v, "job", job);
  bro_template_values_set (v, "device", c->req->source->kind == BRO_SOURCE_DRIVE ? c->req->source->path : "");
  bro_template_values_set (v, "outputDir", c->res->output_dir);
  bro_template_values_set (v, "status", bro_job_state_word (status));
  g_autofree char *manifest = g_build_filename (c->req->job_dir, "manifest.json", NULL);
  bro_template_values_set (v, "manifest", manifest);
  bro_template_values_set (v, "file", c->res->files->len ? c->res->files->pdata[0] : "");
  GString *files = g_string_new (NULL);
  for (guint i = 0; i < c->res->files->len; i++)
    g_string_append_printf (files, "%s%s", i ? " " : "", (char *) c->res->files->pdata[i]);
  bro_template_values_set (v, "files", files->str);
  g_string_free (files, TRUE);
  return v;
}

static void
title_values (GHashTable *v, BroTitle *t, int ordinal, BroDiscInfo *info, const char *file)
{
  int d = bro_title_duration (t);
  g_autofree char *dur = g_strdup_printf ("%d-%02d-%02d", d / 3600, (d / 60) % 60, d % 60);
  g_autofree char *idx = g_strdup_printf ("%d", t->index);
  g_autofree char *n = g_strdup_printf ("%d", ordinal);
  g_autofree char *src = bro_title_source_id (t) >= 0 ? g_strdup_printf ("%d", bro_title_source_id (t)) : g_strdup ("");
  g_autofree char *ch = g_strdup_printf ("%d", bro_title_chapters (t));
  g_autofree char *base = g_path_get_basename (file ? file : bro_title_str (t, BRO_ATTR_OUTPUT_FILE_NAME));
  char *dot = strrchr (base, '.');
  if (dot) *dot = '\0';
  const char *name = bro_title_str (t, BRO_ATTR_NAME);
  bro_template_values_set (v, "title", *name ? name : bro_disc_info_name (info));
  bro_template_values_set (v, "index", idx);
  bro_template_values_set (v, "n", n);
  bro_template_values_set (v, "source", src);
  bro_template_values_set (v, "duration", dur);
  bro_template_values_set (v, "chapters", ch);
  bro_template_values_set (v, "original", base);
  bro_template_values_set (v, "comment", bro_title_str (t, BRO_ATTR_COMMENT));
}

static char *
resolve_output_dir (Ctx *c, GError **error)
{
  const char *root_t = bro_app_config_output_root (c->req->config, c->req->drive);
  g_autofree char *root = NULL;
  g_autoptr (GHashTable) v = base_values (c, BRO_JOB_RUNNING);
  g_autofree char *rel = bro_template_render_path (c->req->drive->output.folder_template, v);
  char *dir;
  gboolean non_empty = FALSE;

  if (!root_t || !*root_t)
    root_t = "~/Videos/Bromelia";
  root = g_str_has_prefix (root_t, "~") ? g_build_filename (g_get_home_dir (), root_t + 1, NULL) : g_strdup (root_t);
  dir = *rel ? g_build_filename (root, rel, NULL) : g_strdup (root);

  if (g_file_test (dir, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
      const char *name;
      while (d && (name = g_dir_read_name (d)))
        if (name[0] != '.')
          non_empty = TRUE;
    }
  if (non_empty)
    {
      switch (c->req->drive->output.conflict_policy)
        {
        case BRO_CONFLICT_UNIQUE:
          {
            char *u = bro_unique_path (dir);
            g_free (dir);
            dir = u;
          }
          break;
        case BRO_CONFLICT_SKIP:
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Output folder %s already exists", dir);
          g_free (dir);
          return NULL;
        default:
          break;
        }
    }
  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create %s", dir);
      g_free (dir);
      return NULL;
    }
  return dir;
}

static GHashTable *
mkv_files (const char *dir)
{
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    if (name[0] != '.' && g_str_has_suffix (name, ".mkv"))
      g_hash_table_add (set, g_build_filename (dir, name, NULL));
  return set;
}

static GArray *
choose_titles (Ctx *c, BroDiscInfo *info, GError **error)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
  if (c->req->manual_titles)
    {
      for (guint i = 0; i < c->req->manual_titles->len; i++)
        {
          int t = g_array_index (c->req->manual_titles, int, i);
          if (bro_disc_info_title (info, t))
            g_array_append_val (out, t);
        }
      if (out->len == 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "None of the chosen titles exist on the disc");
          g_array_unref (out);
          return NULL;
        }
      return out;
    }
  g_autoptr (BroSelectionResult) r = bro_select_titles (info, &c->req->drive->rip.titles);
  if (r->error)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, r->error);
      g_array_unref (out);
      return NULL;
    }
  if (r->requires_manual)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "“%s” is set to choose titles manually. Open the disc and pick titles, "
                   "or change the title rules.", c->req->drive->name);
      g_array_unref (out);
      return NULL;
    }
  for (guint i = 0; i < r->decisions->len; i++)
    {
      BroTitleDecision *d = &g_array_index (r->decisions, BroTitleDecision, i);
      BroTitle *t = bro_disc_info_title (info, d->title_index);
      log_line (c, BRO_SEV_INFO, "Title %d (%s, %d ch): %s%s", d->title_index, t ? bro_title_str (t, BRO_ATTR_DURATION) : "?",
                t ? bro_title_chapters (t) : 0, d->selected ? "selected" : "skipped — ", d->selected ? "" : d->reason);
      if (d->selected)
        g_array_append_val (out, d->title_index);
    }
  if (out->len == 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "No titles matched the title selection rules");
      g_array_unref (out);
      return NULL;
    }
  return out;
}

/* ---- remux ---- */

GPtrArray *
bro_remux_arguments (const char *mkvmerge, GPtrArray *layout_types, BroTitle *title, GHashTable *keep, const char *input, const char *output)
{
  GString *video = g_string_new (NULL), *audio = g_string_new (NULL), *subs = g_string_new (NULL);
  GPtrArray *args;
  static const char *expected[] = { "video", "audio", "subtitles", "other", "other" };

  if (layout_types->len != title->tracks->len)
    goto mismatch;
  for (guint i = 0; i < layout_types->len; i++)
    {
      BroTrack *t = title->tracks->pdata[i];
      BroTrackKind k = bro_track_kind (t);
      if (k != BRO_TRACK_OTHER && !g_str_equal (expected[k], layout_types->pdata[i]))
        goto mismatch;
    }
  for (guint i = 0; i < layout_types->len; i++)
    {
      BroTrack *t = title->tracks->pdata[i];
      const char *ty = layout_types->pdata[i];
      GString *dst = NULL;
      if (!g_hash_table_contains (keep, GINT_TO_POINTER (t->index)))
        continue;
      if (g_str_equal (ty, "video")) dst = video;
      else if (g_str_equal (ty, "audio")) dst = audio;
      else if (g_str_equal (ty, "subtitles")) dst = subs;
      if (dst)
        g_string_append_printf (dst, "%s%u", dst->len ? "," : "", i);
    }
  args = g_ptr_array_new_with_free_func (g_free);
  bro_ptr_array_add_all (args, mkvmerge, "-o", output, NULL);
  if (video->len) bro_ptr_array_add_all (args, "--video-tracks", video->str, NULL); else bro_ptr_array_add_all (args, "--no-video", NULL);
  if (audio->len) bro_ptr_array_add_all (args, "--audio-tracks", audio->str, NULL); else bro_ptr_array_add_all (args, "--no-audio", NULL);
  if (subs->len) bro_ptr_array_add_all (args, "--subtitle-tracks", subs->str, NULL); else bro_ptr_array_add_all (args, "--no-subtitles", NULL);
  g_ptr_array_add (args, g_strdup (input));
  g_string_free (video, TRUE);
  g_string_free (audio, TRUE);
  g_string_free (subs, TRUE);
  return args;

mismatch:
  g_string_free (video, TRUE);
  g_string_free (audio, TRUE);
  g_string_free (subs, TRUE);
  return NULL;
}

static void
collect_line (const char *line, gpointer data)
{
  g_string_append_printf (data, "%s\n", line);
}

static GPtrArray *
identify_tracks (Ctx *c, const char *file)
{
  const char *argv[] = { c->req->mkvmerge, "-J", file, NULL };
  GString *out = g_string_new (NULL);
  int status = -1;
  GPtrArray *types = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (bro_process_run (argv, NULL, NULL, 120, NULL, collect_line, out, &status, NULL, NULL, NULL) && status == 0 &&
      json_parser_load_from_data (parser, out->str, -1, NULL))
    {
      JsonObject *root = json_node_get_object (json_parser_get_root (parser));
      JsonArray *tracks = root && json_object_has_member (root, "tracks") ? json_object_get_array_member (root, "tracks") : NULL;
      if (tracks)
        {
          types = g_ptr_array_new_with_free_func (g_free);
          for (guint i = 0; i < json_array_get_length (tracks); i++)
            g_ptr_array_add (types, g_strdup (json_object_get_string_member (json_array_get_object_element (tracks, i), "type")));
        }
    }
  g_string_free (out, TRUE);
  return types;
}

static void
remux (Ctx *c, const char *file, BroTitle *title, GHashTable *keep)
{
  g_autoptr (GPtrArray) layout = NULL;
  g_autoptr (GPtrArray) args = NULL;
  g_autofree char *dir = g_path_get_dirname (file);
  g_autofree char *uuid = g_uuid_string_random ();
  g_autofree char *tmp_name = g_strdup_printf (".bromelia-%.8s.mkv", uuid);
  g_autofree char *tmp = g_build_filename (dir, tmp_name, NULL);
  g_autofree char *base = g_path_get_basename (file);
  int status = -1;

  if (!c->req->mkvmerge || g_hash_table_size (keep) == title->tracks->len)
    return;
  phase (c, "Removing unselected tracks");
  layout = identify_tracks (c, file);
  if (!layout)
    {
      log_line (c, BRO_SEV_WARNING, "mkvmerge could not read %s; keeping all tracks", base);
      return;
    }
  args = bro_remux_arguments (c->req->mkvmerge, layout, title, keep, file, tmp);
  if (!args)
    {
      log_line (c, BRO_SEV_WARNING, "Track layout of %s differs from the disc listing; keeping all tracks", base);
      return;
    }
  g_ptr_array_add (args, NULL);
  g_autofree char *cmd = bro_command_line ((const char *const *) args->pdata);
  log_line (c, BRO_SEV_INFO, "$ %s", cmd);
  /* mkvmerge exits with 1 for warnings; the output is still valid. */
  if (bro_process_run ((const char *const *) args->pdata, NULL, NULL, 0, c->req->cancellable, NULL, NULL, &status, NULL, NULL, NULL) &&
      status >= 0 && status <= 1 && g_file_test (tmp, G_FILE_TEST_EXISTS) && g_rename (tmp, file) == 0)
    log_line (c, BRO_SEV_INFO, "Kept %u of %u tracks in %s", g_hash_table_size (keep), title->tracks->len, base);
  else
    {
      g_unlink (tmp);
      log_line (c, BRO_SEV_WARNING, "mkvmerge failed; keeping all tracks in %s", base);
    }
}

static char *
rename_file (Ctx *c, const char *file, BroTitle *title, int ordinal, BroDiscInfo *info, const char *out_dir)
{
  const char *explicit_name = g_hash_table_lookup (c->req->name_overrides, GINT_TO_POINTER (title->index));
  const char *tmpl = explicit_name && *explicit_name ? explicit_name : c->req->drive->output.file_name_template;
  g_autoptr (GHashTable) v = NULL;
  g_autofree char *rel = NULL;
  g_autofree char *target = NULL;
  g_autofree char *base = g_path_get_basename (file);
  char *unique;

  if (!tmpl || !*tmpl)
    return g_strdup (file);
  v = base_values (c, BRO_JOB_RUNNING);
  title_values (v, title, ordinal, info, file);
  rel = bro_template_render_path (tmpl, v);
  if (!*rel)
    return g_strdup (file);
  if (!g_str_has_suffix (rel, ".mkv"))
    {
      char *r = g_strconcat (rel, ".mkv", NULL);
      g_free (rel);
      rel = r;
    }
  target = g_build_filename (out_dir, rel, NULL);
  if (g_str_equal (target, file))
    return g_strdup (file);
  unique = bro_unique_path (target);
  {
    g_autofree char *parent = g_path_get_dirname (unique);
    g_mkdir_with_parents (parent, 0755);
  }
  if (g_rename (file, unique) != 0)
    {
      log_line (c, BRO_SEV_WARNING, "Could not rename %s", base);
      g_free (unique);
      return g_strdup (file);
    }
  log_line (c, BRO_SEV_INFO, "Renamed %s → %s", base, rel);
  return unique;
}

static gboolean
rip_titles (Ctx *c, const BroSource *src, BroDiscInfo *info, const char *out_dir, int extra_steps, GError **error)
{
  g_autoptr (GArray) indices = choose_titles (c, info, error);
  g_autoptr (GPtrArray) failures = g_ptr_array_new_with_free_func (g_free);
  gboolean every = TRUE, single;
  int count;

  if (!indices)
    return FALSE;
  if (c->rip_titles)
    g_array_unref (c->rip_titles);
  c->rip_titles = g_array_ref (indices);
  if (indices->len != info->titles->len)
    every = FALSE;
  single = every && g_hash_table_size (c->req->track_selections) == 0;
  count = single ? 1 : (int) indices->len;
  steps (c, extra_steps, extra_steps + count);

  for (int n = 0; n < count; n++)
    {
      int ti = single ? -1 : g_array_index (indices, int, n);
      g_autofree char *tstr = single ? g_strdup ("all") : g_strdup_printf ("%d", ti);
      g_autofree char *ph = single ? g_strdup_printf ("Ripping %u title(s)", indices->len)
                                   : g_strdup_printf ("Ripping title %d (%d of %d)", ti, n + 1, count);
      g_autoptr (GHashTable) before = mkv_files (out_dir);
      g_autoptr (GHashTable) after = NULL;
      g_autoptr (GPtrArray) produced = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GPtrArray) args = NULL;
      GHashTableIter it;
      gpointer key;
      Summary *s;

      if (cancelled (c))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          return FALSE;
        }
      steps (c, extra_steps + n, extra_steps + count);
      update (c, BRO_UPDATE_PROGRESS, BRO_SEV_INFO, NULL, 0, 0);
      phase (c, ph);
      c->env = (!single && g_hash_table_contains (c->req->track_selections, GINT_TO_POINTER (ti)) && c->all_env) ? c->all_env : c->base_env;
      args = bro_makemkv_mkv_args (c->env, src, tstr, out_dir, &c->req->drive->rip);
      s = run_makemkv (c, args, error);
      c->env = c->base_env;
      if (!s)
        return FALSE;

      after = mkv_files (out_dir);
      g_hash_table_iter_init (&it, after);
      while (g_hash_table_iter_next (&it, &key, NULL))
        if (!g_hash_table_contains (before, key))
          g_ptr_array_add (produced, g_strdup (key));
      g_ptr_array_sort (produced, (GCompareFunc) g_strcmp0);

      if (s->exit_status != 0 || s->failed > 0 || produced->len == 0)
        {
          const char *why = s->errors->len ? g_ptr_array_index (s->errors, s->errors->len - 1) : NULL;
          g_autofree char *why_s = why ? g_strdup (why) : g_strdup_printf ("makemkvcon exit status %d", s->exit_status);
          g_ptr_array_add (failures, single ? g_strdup (why_s) : g_strdup_printf ("title %s: %s", tstr, why_s));
          log_line (c, BRO_SEV_ERROR, "Title %s failed: %s", tstr, why_s);
        }
      for (guint i = 0; i < produced->len; i++)
        {
          const char *file = produced->pdata[i];
          int index = ti;
          BroTitle *title;
          if (single)
            {
              g_autofree char *base = g_path_get_basename (file);
              index = -1;
              for (guint k = 0; k < info->titles->len; k++)
                if (g_str_equal (bro_title_str (info->titles->pdata[k], BRO_ATTR_OUTPUT_FILE_NAME), base))
                  index = ((BroTitle *) info->titles->pdata[k])->index;
            }
          title = index >= 0 ? bro_disc_info_title (info, index) : NULL;
          char *final = g_strdup (file);
          if (title)
            {
              GHashTable *keep = g_hash_table_lookup (c->req->track_selections, GINT_TO_POINTER (index));
              int ordinal = 1;
              if (keep)
                remux (c, final, title, keep);
              for (guint k = 0; k < indices->len; k++)
                if (g_array_index (indices, int, k) == index)
                  ordinal = (int) k + 1;
              char *renamed = rename_file (c, final, title, ordinal, info, out_dir);
              g_free (final);
              final = renamed;
            }
          g_ptr_array_add (c->res->files, final);
          update (c, BRO_UPDATE_FILE, BRO_SEV_INFO, final, 0, 0);
        }
      summary_free (s);
      if (single && produced->len == 0)
        break;
    }
  steps (c, extra_steps + count, extra_steps + count);
  if (failures->len)
    {
      g_ptr_array_add (failures, NULL);
      g_autofree char *joined = g_strjoinv ("; ", (char **) failures->pdata);
      if (failures->len == 2)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Rip failed: %s", joined);
      else
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%u titles failed: %s", failures->len - 1, joined);
      return FALSE;
    }
  return TRUE;
}

static char *
backup (Ctx *c, gboolean decrypt, const char *out_dir, gboolean in_subfolder, GError **error)
{
  BroSource *src = c->req->source;
  g_autofree char *dest = g_strdup (out_dir);
  g_autoptr (GPtrArray) args = NULL;
  Summary *s;
  gboolean exists;

  if (src->kind != BRO_SOURCE_DRIVE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Backups can only be made from a disc in a drive");
      return NULL;
    }
  if (in_subfolder)
    {
      g_autoptr (GHashTable) v = base_values (c, BRO_JOB_RUNNING);
      g_autofree char *sub = bro_template_render_path (c->req->drive->output.backup_subfolder, v);
      g_free (dest);
      dest = g_build_filename (out_dir, *sub ? sub : "backup", NULL);
    }
  g_mkdir_with_parents (dest, 0755);
  if (c->req->drive->rip.backup_format == BRO_BACKUP_ISO)
    {
      g_autofree char *name = bro_sanitize_component (c->res->disc_label && *c->res->disc_label ? c->res->disc_label : "disc");
      g_autofree char *iso_name = g_strconcat (name, ".iso", NULL);
      g_autofree char *iso = g_build_filename (dest, iso_name, NULL);
      g_free (dest);
      dest = bro_unique_path (iso);
    }
  phase (c, decrypt ? "Backing up disc (decrypted)" : "Backing up disc");
  update (c, BRO_UPDATE_PROGRESS, BRO_SEV_INFO, NULL, 0, 0);
  c->expected_index = src->index;
  g_free (c->expected_device);
  c->expected_device = g_strdup (src->path);
  args = bro_makemkv_backup_args (c->env, src, decrypt, dest, &c->req->drive->rip);
  s = run_makemkv (c, args, error);
  g_clear_pointer (&c->expected_device, g_free);
  if (!s)
    return NULL;
  exists = g_file_test (dest, G_FILE_TEST_IS_REGULAR);
  if (!exists && g_file_test (dest, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) d = g_dir_open (dest, 0, NULL);
      exists = d && g_dir_read_name (d) != NULL;
    }
  if (s->exit_status != 0 || !exists || s->failed > 0)
    {
      const char *why = s->errors->len ? g_ptr_array_index (s->errors, s->errors->len - 1) : NULL;
      if (why)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Backup failed: %s", why);
      else
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Backup failed: makemkvcon exit status %d", s->exit_status);
      summary_free (s);
      return NULL;
    }
  summary_free (s);
  log_line (c, BRO_SEV_INFO, "Backup saved to %s", dest);
  return g_steal_pointer (&dest);
}

static char *
title_key (BroTitle *t)
{
  return g_strdup_printf ("%d|%d|%s", bro_title_source_id (t), bro_title_duration (t), bro_title_str (t, BRO_ATTR_SEGMENTS_MAP));
}

/* Maps title choices made on the disc listing to the listing of the backup copy. */
static void
remap_selections (Ctx *c, BroDiscInfo *disc, BroDiscInfo *bk)
{
  g_autoptr (GHashTable) map = g_hash_table_new (g_direct_hash, g_direct_equal);
  if (!disc)
    return;
  for (guint i = 0; i < disc->titles->len; i++)
    {
      BroTitle *t = disc->titles->pdata[i];
      g_autofree char *k = title_key (t);
      BroTitle *found = NULL;
      for (guint j = 0; j < bk->titles->len && !found; j++)
        {
          g_autofree char *k2 = title_key (bk->titles->pdata[j]);
          if (g_str_equal (k, k2))
            found = bk->titles->pdata[j];
        }
      if (!found)
        found = bro_disc_info_title (bk, t->index);
      if (found)
        g_hash_table_insert (map, GINT_TO_POINTER (t->index), GINT_TO_POINTER (found->index + 1));
    }
#define MAPPED(i) (GPOINTER_TO_INT (g_hash_table_lookup (map, GINT_TO_POINTER (i))) - 1)
  if (c->req->manual_titles)
    {
      GArray *m = g_array_new (FALSE, FALSE, sizeof (int));
      for (guint i = 0; i < c->req->manual_titles->len; i++)
        {
          int v = MAPPED (g_array_index (c->req->manual_titles, int, i));
          if (v >= 0)
            g_array_append_val (m, v);
        }
      g_array_unref (c->req->manual_titles);
      c->req->manual_titles = m;
    }
  {
    GHashTable *tracks = bro_track_selections_new ();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, c->req->track_selections);
    while (g_hash_table_iter_next (&it, &k, &v))
      if (MAPPED (GPOINTER_TO_INT (k)) >= 0)
        g_hash_table_insert (tracks, GINT_TO_POINTER (MAPPED (GPOINTER_TO_INT (k))), g_hash_table_ref (v));
    g_hash_table_unref (c->req->track_selections);
    c->req->track_selections = tracks;
  }
  {
    GHashTable *names = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, c->req->name_overrides);
    while (g_hash_table_iter_next (&it, &k, &v))
      if (MAPPED (GPOINTER_TO_INT (k)) >= 0)
        g_hash_table_insert (names, GINT_TO_POINTER (MAPPED (GPOINTER_TO_INT (k))), g_strdup (v));
    g_hash_table_unref (c->req->name_overrides);
    c->req->name_overrides = names;
  }
#undef MAPPED
}

static gboolean
write_disc_info (BroDiscInfo *info, const char *dir, char **path_out)
{
  g_autofree char *json = bro_disc_info_to_json (info);
  char *path = g_build_filename (dir, "disc-info.json", NULL);
  gboolean ok = g_file_set_contents (path, json, -1, NULL);
  if (path_out) *path_out = path; else g_free (path);
  return ok;
}

static gboolean
execute (Ctx *c, GError **error)
{
  BroRunRequest *req = c->req;
  gboolean needs_all = bro_rip_mode_makes_mkv (req->mode) && g_hash_table_size (req->track_selections) > 0;
  g_autofree char *home = g_build_filename (req->job_dir, "home", NULL);
  BroDiscInfo *info = req->preloaded ? bro_disc_info_ref (req->preloaded) : NULL;
  gboolean need_info;
  g_autofree char *out_dir = NULL;

  if (needs_all && !req->mkvmerge)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Choosing individual tracks requires mkvmerge (MKVToolNix). Install MKVToolNix or reset the track choices.");
      if (info) bro_disc_info_unref (info);
      return FALSE;
    }
  c->base_env = c->env = bro_makemkv_env_prepare (req->makemkvcon, req->config, req->drive, home, NULL, error);
  if (!c->env)
    {
      if (info) bro_disc_info_unref (info);
      return FALSE;
    }
  if (needs_all)
    {
      g_autofree char *home2 = g_build_filename (req->job_dir, "home-alltracks", NULL);
      c->all_env = bro_makemkv_env_prepare (req->makemkvcon, req->config, req->drive, home2, "+sel:all", error);
      if (!c->all_env)
        {
          if (info) bro_disc_info_unref (info);
          return FALSE;
        }
    }
  c->show_debug = g_strcmp0 (g_hash_table_lookup (c->env->settings, "app_ShowDebug"), "1") == 0;
  log_line (c, BRO_SEV_INFO, "MakeMKV home: %s", home);
  if (c->env->profile_path)
    log_line (c, BRO_SEV_INFO, "Profile: %s", c->env->profile_path);

  need_info = bro_rip_mode_makes_mkv (req->mode) || req->mode == BRO_MODE_INFO_ONLY || !c->res->disc_label || !*c->res->disc_label;
  if (!info && need_info)
    {
      info = scan_disc (c, req->source, error);
      if (!info)
        return FALSE;
    }
  c->res->info = info;
  if (info && *bro_disc_info_name (info))
    {
      g_free (c->res->disc_label);
      c->res->disc_label = g_strdup (bro_disc_info_name (info));
      update (c, BRO_UPDATE_DISC_LABEL, BRO_SEV_INFO, c->res->disc_label, 0, 0);
    }

  out_dir = resolve_output_dir (c, error);
  if (!out_dir)
    return FALSE;
  c->res->output_dir = g_strdup (out_dir);
  update (c, BRO_UPDATE_OUTPUT_DIR, BRO_SEV_INFO, out_dir, 0, 0);
  log_line (c, BRO_SEV_INFO, "Output folder: %s", out_dir);

  switch (req->mode)
    {
    case BRO_MODE_INFO_ONLY:
      {
        char *path = NULL;
        if (!info || !write_disc_info (info, out_dir, &path))
          {
            g_free (path);
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not write disc information");
            return FALSE;
          }
        g_ptr_array_add (c->res->files, path);
        return TRUE;
      }
    case BRO_MODE_MKV:
      if (req->drive->rip.write_disc_info_json)
        write_disc_info (info, out_dir, NULL);
      return rip_titles (c, req->source, info, out_dir, 0, error);
    case BRO_MODE_BACKUP:
    case BRO_MODE_BACKUP_DECRYPTED:
      {
        char *dest;
        steps (c, 0, 1);
        dest = backup (c, req->mode == BRO_MODE_BACKUP_DECRYPTED, out_dir, FALSE, error);
        if (!dest)
          return FALSE;
        g_ptr_array_add (c->res->files, dest);
        return TRUE;
      }
    case BRO_MODE_BACKUP_THEN_MKV:
      {
        g_autofree char *dest = NULL;
        g_autoptr (BroSource) bsrc = NULL;
        BroDiscInfo *binfo;
        gboolean ok;
        steps (c, 0, 2);
        dest = backup (c, TRUE, out_dir, TRUE, error);
        if (!dest)
          return FALSE;
        steps (c, 1, 2);
        bsrc = bro_source_new_path (req->drive->rip.backup_format == BRO_BACKUP_ISO ? BRO_SOURCE_ISO : BRO_SOURCE_FOLDER, dest);
        binfo = scan_disc (c, bsrc, error);
        if (!binfo)
          return FALSE;
        remap_selections (c, info, binfo);
        if (req->drive->rip.write_disc_info_json)
          write_disc_info (binfo, out_dir, NULL);
        ok = rip_titles (c, bsrc, binfo, out_dir, 1, error);
        bro_disc_info_unref (binfo);
        if (!ok)
          return FALSE;
        if (req->drive->rip.keep_backup_after_mkv)
          g_ptr_array_add (c->res->files, g_strdup (dest));
        else
          {
            g_autoptr (GFile) f = g_file_new_for_path (dest);
            g_autofree char *cmd_argv0 = g_find_program_in_path ("rm");
            log_line (c, BRO_SEV_INFO, "Removing backup %s", dest);
            if (cmd_argv0)
              {
                const char *argv[] = { cmd_argv0, "-rf", "--", dest, NULL };
                bro_process_run (argv, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
              }
            else
              g_file_delete (f, NULL, NULL);
          }
        return TRUE;
      }
    }
  return TRUE;
}

/* ---- post-processing ---- */

gboolean
bro_post_step_should_run (const BroPostStep *step, BroJobState status)
{
  if (!step->enabled || !step->executable || !*g_strstrip (step->executable))
    return FALSE;
  switch (step->run_on)
    {
    case BRO_RUN_ALWAYS: return TRUE;
    case BRO_RUN_SUCCESS: return status == BRO_JOB_SUCCEEDED;
    default: return status == BRO_JOB_FAILED || status == BRO_JOB_CANCELLED;
    }
}

static char *
expand_home (const char *p)
{
  if (p && (g_str_equal (p, "~") || g_str_has_prefix (p, "~/")))
    return g_build_filename (g_get_home_dir (), p + 1, NULL);
  return g_strdup (p ? p : "");
}

GPtrArray *
bro_post_step_argv (const BroPostStep *step, GHashTable *values, GPtrArray *files)
{
  GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) tokens = bro_split_arguments (step->arguments);
  g_autofree char *exe_r = bro_template_render (step->executable, values, FALSE);
  g_autofree char *exe = expand_home (exe_r);
  g_autofree char *interp = expand_home (g_strstrip (g_strdup (step->interpreter ? step->interpreter : "")));

  if (*interp)
    {
      g_ptr_array_add (argv, g_strdup (interp));
      g_ptr_array_add (argv, g_strdup (exe));
    }
  else if (g_file_test (exe, G_FILE_TEST_EXISTS) && !g_file_test (exe, G_FILE_TEST_IS_EXECUTABLE))
    {
      /* A script without the executable bit: run it through the shell. */
      g_ptr_array_add (argv, g_strdup ("/bin/sh"));
      g_ptr_array_add (argv, g_strdup (exe));
    }
  else
    g_ptr_array_add (argv, g_strdup (exe));

  for (guint i = 0; i < tokens->len; i++)
    {
      const char *t = tokens->pdata[i];
      if (g_str_equal (t, "{files}"))
        {
          for (guint j = 0; files && j < files->len; j++)
            g_ptr_array_add (argv, g_strdup (files->pdata[j]));
        }
      else
        g_ptr_array_add (argv, bro_template_render (t, values, FALSE));
    }
  return argv;
}

typedef struct {
  Ctx *c;
  const char *name;
} StepLog;

static void
on_step_line (const char *line, gpointer data)
{
  StepLog *sl = data;
  log_line (sl->c, BRO_SEV_INFO, "  [%s] %s", sl->name, line);
}

static gboolean
run_post_processing (Ctx *c, BroJobState status)
{
  gboolean failed = FALSE;
  GPtrArray *steps_arr = c->req->drive->post_process;
  g_auto (GStrv) base_env = g_get_environ ();
  g_autofree char *manifest = g_build_filename (c->req->job_dir, "manifest.json", NULL);
  g_autofree char *logpath = g_build_filename (c->req->job_dir, "log.txt", NULL);
  g_autofree char *count = g_strdup_printf ("%u", c->res->files->len);
  g_autofree char *src = bro_source_info_argument (c->req->source);
  GString *files = g_string_new (NULL);

  for (guint i = 0; i < c->res->files->len; i++)
    g_string_append_printf (files, "%s%s", i ? "\n" : "", (char *) c->res->files->pdata[i]);
#define SETENV(k, v) base_env = g_environ_setenv (base_env, k, (v) ? (v) : "", TRUE)
  SETENV ("BROMELIA_JOB_ID", c->req->job_id);
  SETENV ("BROMELIA_STATUS", bro_job_state_word (status));
  SETENV ("BROMELIA_MODE", bro_rip_mode_to_string (c->req->mode));
  SETENV ("BROMELIA_DRIVE_NAME", c->req->drive->name);
  SETENV ("BROMELIA_DRIVE_ID", c->req->drive->id);
  SETENV ("BROMELIA_DISC_NAME", c->res->disc_label);
  SETENV ("BROMELIA_DISC_TYPE", bro_disc_info_type_token (c->res->info));
  SETENV ("BROMELIA_OUTPUT_DIR", c->res->output_dir);
  SETENV ("BROMELIA_FILES", files->str);
  SETENV ("BROMELIA_FILE_COUNT", count);
  SETENV ("BROMELIA_MANIFEST", manifest);
  SETENV ("BROMELIA_LOG", logpath);
  SETENV ("BROMELIA_SOURCE", src);
  if (c->req->source->kind == BRO_SOURCE_DRIVE)
    SETENV ("BROMELIA_DEVICE", c->req->source->path);
  if (c->res->error)
    SETENV ("BROMELIA_ERROR", c->res->error);
#undef SETENV
  g_string_free (files, TRUE);

  for (guint i = 0; i < steps_arr->len; i++)
    {
      BroPostStep *step = steps_arr->pdata[i];
      guint targets = step->per_file ? c->res->files->len : 1;
      if (!bro_post_step_should_run (step, status))
        continue;
      if (!targets)
        continue;
      phase (c, "Post-processing");
      for (guint t = 0; t < targets; t++)
        {
          const char *file = step->per_file ? c->res->files->pdata[t] : NULL;
          g_autoptr (GHashTable) values = base_values (c, status);
          g_auto (GStrv) envp = g_strdupv (base_env);
          g_autoptr (GPtrArray) argv = NULL;
          g_autoptr (GError) err = NULL;
          g_autofree char *cmd = NULL;
          g_autofree char *wd = NULL;
          int exit_status = -1;
          gboolean timed_out = FALSE;
          GHashTableIter it;
          gpointer k, v;
          StepLog sl = { c, step->name };

          if (cancelled (c))
            return failed;
          if (file)
            {
              g_autofree char *fname = g_path_get_basename (file);
              g_autofree char *stem = g_strdup (fname);
              char *dot = strrchr (stem, '.');
              if (dot) *dot = '\0';
              bro_template_values_set (values, "file", file);
              bro_template_values_set (values, "filename", fname);
              bro_template_values_set (values, "stem", stem);
              envp = g_environ_setenv (envp, "BROMELIA_FILE", file, TRUE);
            }
          g_hash_table_iter_init (&it, step->environment);
          while (g_hash_table_iter_next (&it, &k, &v))
            {
              g_autofree char *rv = bro_template_render (v, values, FALSE);
              envp = g_environ_setenv (envp, k, rv, TRUE);
            }
          argv = bro_post_step_argv (step, values, c->res->files);
          g_ptr_array_add (argv, NULL);
          cmd = bro_command_line ((const char *const *) argv->pdata);
          if (step->working_directory && *step->working_directory)
            {
              g_autofree char *r = bro_template_render (step->working_directory, values, FALSE);
              wd = expand_home (r);
            }
          else
            wd = g_strdup (c->res->output_dir);
          log_line (c, BRO_SEV_INFO, "▶ %s: %s", step->name, cmd);
          if (!bro_process_run ((const char *const *) argv->pdata, (const char *const *) envp, wd, step->timeout_seconds,
                                c->req->cancellable, on_step_line, &sl, &exit_status, NULL, &timed_out, &err))
            {
              log_line (c, step->fail_job_on_error ? BRO_SEV_ERROR : BRO_SEV_WARNING, "%s could not be started: %s",
                        step->name, err ? err->message : "unknown error");
              if (step->fail_job_on_error)
                failed = TRUE;
              continue;
            }
          if (timed_out)
            log_line (c, BRO_SEV_WARNING, "%s timed out after %d s", step->name, step->timeout_seconds);
          else
            log_line (c, exit_status == 0 ? BRO_SEV_INFO : (step->fail_job_on_error ? BRO_SEV_ERROR : BRO_SEV_WARNING),
                      "%s exited with status %d", step->name, exit_status);
          if ((exit_status != 0 || timed_out) && step->fail_job_on_error)
            failed = TRUE;
        }
    }
  return failed;
}

static void
write_manifest (Ctx *c, BroJobState status)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *text = NULL;
  g_autofree char *path = g_build_filename (c->req->job_dir, "manifest.json", NULL);
  g_autofree char *src = bro_source_info_argument (c->req->source);

  json_builder_begin_object (b);
#define S(k, v) do { json_builder_set_member_name (b, k); json_builder_add_string_value (b, (v) ? (v) : ""); } while (0)
  S ("jobId", c->req->job_id);
  S ("status", bro_job_state_word (status));
  S ("mode", bro_rip_mode_to_string (c->req->mode));
  S ("driveName", c->req->drive->name);
  S ("driveId", c->req->drive->id);
  S ("devicePath", c->req->source->kind == BRO_SOURCE_DRIVE ? c->req->source->path : "");
  S ("source", src);
  S ("discName", c->res->disc_label);
  S ("discType", bro_disc_info_type_token (c->res->info));
  S ("outputDirectory", c->res->output_dir);
  json_builder_set_member_name (b, "files");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->res->files->len; i++)
    json_builder_add_string_value (b, c->res->files->pdata[i]);
  json_builder_end_array (b);
  json_builder_set_member_name (b, "titles");
  json_builder_begin_array (b);
  for (guint i = 0; c->rip_titles && c->res->info && i < c->rip_titles->len; i++)
    {
      BroTitle *t = bro_disc_info_title (c->res->info, g_array_index (c->rip_titles, int, i));
      if (!t)
        continue;
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "index");
      json_builder_add_int_value (b, t->index);
      S ("name", bro_title_str (t, BRO_ATTR_NAME));
      S ("duration", bro_title_str (t, BRO_ATTR_DURATION));
      json_builder_set_member_name (b, "chapters");
      json_builder_add_int_value (b, bro_title_chapters (t));
      if (bro_title_source_id (t) >= 0)
        {
          json_builder_set_member_name (b, "sourceTitleId");
          json_builder_add_int_value (b, bro_title_source_id (t));
        }
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  if (c->res->error)
    S ("error", c->res->error);
#undef S
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  text = bro_json_to_string (root, TRUE);
  g_file_set_contents (path, text, -1, NULL);
}

static gboolean
eject_device (const char *device)
{
  g_autofree char *eject = g_find_program_in_path ("eject");
  int status = -1;
  if (!device || !*device)
    return FALSE;
  if (eject)
    {
      const char *argv[] = { eject, device, NULL };
      if (bro_process_run (argv, NULL, NULL, 60, NULL, NULL, NULL, &status, NULL, NULL, NULL) && status == 0)
        return TRUE;
    }
#ifdef __APPLE__
  {
    const char *argv[] = { "/usr/sbin/diskutil", "eject", device, NULL };
    if (bro_process_run (argv, NULL, NULL, 60, NULL, NULL, NULL, &status, NULL, NULL, NULL) && status == 0)
      return TRUE;
  }
#endif
  return FALSE;
}

BroRunResult *
bro_run_job (BroRunRequest *req)
{
  Ctx c = { 0 };
  BroRunResult *res = g_new0 (BroRunResult, 1);
  g_autoptr (GError) error = NULL;
  g_autofree char *logpath = NULL;
  BroJobState status = BRO_JOB_SUCCEEDED;

  res->files = g_ptr_array_new_with_free_func (g_free);
  res->disc_label = g_strdup (req->disc_label ? req->disc_label : "");
  res->started_at = g_get_real_time () / G_USEC_PER_SEC;
  c.req = req;
  c.res = res;
  c.commands = g_ptr_array_new_with_free_func (g_free);
  c.step_count = 1;
  g_mkdir_with_parents (req->job_dir, 0755);
  logpath = g_build_filename (req->job_dir, "log.txt", NULL);
  c.log = fopen (logpath, "a");

  {
    g_autofree char *src = bro_source_display_name (req->source);
    log_line (&c, BRO_SEV_INFO, "Bromelia job %s", req->job_id);
    log_line (&c, BRO_SEV_INFO, "%s from %s using configuration “%s”", bro_rip_mode_label (req->mode), src, req->drive->name);
  }

  if (!execute (&c, &error))
    {
      if (g_cancellable_is_cancelled (req->cancellable) && (!error || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
        {
          status = BRO_JOB_CANCELLED;
          res->error = g_strdup ("Cancelled by user");
          log_line (&c, BRO_SEV_WARNING, "Job cancelled");
        }
      else
        {
          status = BRO_JOB_FAILED;
          res->error = g_strdup (error ? error->message : "Unknown error");
          log_line (&c, BRO_SEV_ERROR, "%s", res->error);
        }
    }
  else if (g_cancellable_is_cancelled (req->cancellable))
    status = BRO_JOB_CANCELLED;

  write_manifest (&c, status);
  if (!g_cancellable_is_cancelled (req->cancellable) && run_post_processing (&c, status) && status == BRO_JOB_SUCCEEDED)
    {
      status = BRO_JOB_FAILED;
      g_free (res->error);
      res->error = g_strdup ("A post-processing step failed");
    }

  if (!req->skip_eject && req->source->kind == BRO_SOURCE_DRIVE &&
      ((status == BRO_JOB_SUCCEEDED && req->drive->automation.eject_when_done) ||
       (status == BRO_JOB_FAILED && req->drive->automation.eject_on_failure)))
    {
      phase (&c, "Ejecting");
      if (eject_device (req->source->path))
        log_line (&c, BRO_SEV_INFO, "Disc ejected");
      else
        log_line (&c, BRO_SEV_WARNING, "Could not eject %s", req->source->path);
    }

  res->status = status;
  res->finished_at = g_get_real_time () / G_USEC_PER_SEC;
  write_manifest (&c, status);
  log_line (&c, BRO_SEV_INFO, "Finished: %s", bro_job_state_label (status));
  if (c.log)
    fclose (c.log);
  if (c.base_env) bro_makemkv_env_free (c.base_env);
  if (c.all_env) bro_makemkv_env_free (c.all_env);
  if (c.rip_titles) g_array_unref (c.rip_titles);
  g_free (c.last_total);
  g_free (c.expected_device);
  g_ptr_array_unref (c.commands);
  return res;
}
