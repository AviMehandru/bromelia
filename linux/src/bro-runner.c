/* bro-runner.c — job pipeline, post-processing and mkvmerge remuxing. */
#include "bro-runner.h"
#include "bro-dvd.h"
#include "bro-identity.h"
#include "bro-logic.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

const char *
bro_job_state_label (BroJobState s)
{
  switch (s)
    {
    case BRO_JOB_QUEUED: return "Queued";
    case BRO_JOB_WAITING: return "Starting soon";
    case BRO_JOB_RUNNING: return "Running";
    case BRO_JOB_SUCCEEDED: return "Completed";
    case BRO_JOB_COMPLETED_WITH_ERRORS: return "Completed with read errors";
    case BRO_JOB_FAILED: return "Failed";
    default: return "Cancelled";
    }
}

const char *
bro_job_state_word (BroJobState s)
{
  return s == BRO_JOB_SUCCEEDED ? "success" : s == BRO_JOB_COMPLETED_WITH_ERRORS ? "errors" : s == BRO_JOB_CANCELLED ? "cancelled" : "failed";
}

gboolean
bro_job_state_finished (BroJobState s)
{
  return s == BRO_JOB_SUCCEEDED || s == BRO_JOB_COMPLETED_WITH_ERRORS || s == BRO_JOB_FAILED || s == BRO_JOB_CANCELLED;
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
  r->media_name = g_strdup ("");
  r->media_kind = -1;
  r->first_episode = -1;
  r->disc_flags = -1;
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
  g_free (r->media_name);
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
  /* identity, episodes and archiving */
  BroIdentity *identity;
  BroVideoTS *videots;
  BroEpisodePlan *plan;        /* "play all" title to split */
  GHashTable *separate;        /* MakeMKV title -> episode offset + 1 */
  int first_episode, episode_width;
  GHashTable *file_titles;     /* char* path -> title index + 1 */
  GPtrArray *episodes;         /* EpisodeRec* */
  GPtrArray *checksums;        /* BroChecksum* */
  char *checksum_file;
  char *makemkv_version;
  GPtrArray *error_messages;   /* char* */
  /* output (see finish_output) */
  char *output_dir;            /* final folder, reserved when the job starts */
  char *work_dir;              /* hidden staging folder inside it */
  gboolean owns_output_dir;    /* created for this job, so it may be renamed or removed */
  gboolean collect_data_errors; /* errors of the running makemkvcon are read errors (rips, backups) */
  gboolean reported_missing_verifier;
  GPtrArray *data_errors;      /* char*: errors MakeMKV reported while ripping or backing up */
} Ctx;

typedef struct {
  char *file; /* relative to the output folder */
  int episode, source, first, last;
} EpisodeRec;

static void
episode_rec_free (EpisodeRec *e)
{
  g_free (e->file);
  g_free (e);
}

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
          {
            g_ptr_array_add (s->errors, g_strdup (ev->text));
            g_ptr_array_add (c->error_messages, g_strdup (ev->text));
            if (c->collect_data_errors)
              g_ptr_array_add (c->data_errors, g_strdup (ev->text));
          }
        if (ev->code == 1005 && !c->makemkv_version)
          c->makemkv_version = g_strdup (ev->params->len ? ev->params->pdata[0] : ev->text);
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

/* Runs makemkvcon with the current environment. Returns NULL (with error) when it could not run or was cancelled.
 * With reads_data, error messages count as read errors (rips and backups, not listings). */
static Summary *
run_makemkv (Ctx *c, GPtrArray *args, gboolean reads_data, GError **error)
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
  c->collect_data_errors = reads_data;
  if (!bro_process_run ((const char *const *) args->pdata, (const char *const *) envp, c->env->home, 0, c->req->cancellable,
                        on_makemkv_line, c, &s->exit_status, &was_cancelled, NULL, error))
    {
      c->collect_data_errors = FALSE;
      c->sum = NULL;
      summary_free (s);
      if (error && *error && !g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_prefix_error (error, "Could not start makemkvcon: ");
      return NULL;
    }
  c->sum = NULL;
  c->collect_data_errors = FALSE;
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
  s = run_makemkv (c, args, FALSE, error);
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
  if (c->identity)
    bro_identity_template_values (c->identity, v, bro_rip_mode_makes_mkv (c->req->mode) ? "Rip" : "Backup");
  else
    {
      BroIdentity *id = bro_identity_resolve (info, c->res->disc_label, c->req->disc_flags, -1, c->req->mode == BRO_MODE_BACKUP,
                                              c->req->media_name, c->req->media_kind, 0);
      bro_identity_template_values (id, v, bro_rip_mode_makes_mkv (c->req->mode) ? "Rip" : "Backup");
      bro_identity_free (id);
    }
  bro_template_values_set (v, "checksums", c->checksum_file ? c->checksum_file : "");
  return v;
}

static void
resolve_identity (Ctx *c, BroDiscInfo *info, int play_all, int format)
{
  int fmt = format >= 0 ? format : (c->identity ? (int) c->identity->format : -1);
  BroIdentity *id = bro_identity_resolve (info, c->res->disc_label, c->req->disc_flags, fmt, c->req->mode == BRO_MODE_BACKUP,
                                          c->req->media_name, c->req->media_kind, play_all);
  if (!bro_identity_equal (id, c->identity))
    {
      g_autofree char *set = bro_label_set_description (&id->label);
      g_autofree char *code = bro_identity_format_code (id);
      g_autofree char *paren = *set ? g_strdup_printf (" (%s)", set) : g_strdup ("");
      log_line (c, BRO_SEV_INFO, "Identified as %s “%s”%s, %s, format code %s — %s", id->kind == BRO_KIND_TV ? "TV show" : "movie",
                id->name, paren, bro_format_label (id->format), code, id->reason);
    }
  bro_identity_free (c->identity);
  c->identity = id;
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
  /* Only a folder made for this disc may be renamed or removed; never the output root itself. */
  gboolean owned = *rel != '\0';

  if (!root_t || !*root_t)
    root_t = "~/Videos/Bromelia";
  root = g_str_has_prefix (root_t, "~") ? g_build_filename (g_get_home_dir (), root_t + 1, NULL) : g_strdup (root_t);
  dir = *rel ? g_build_filename (root, rel, NULL) : g_strdup (root);

  if (g_file_test (dir, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
      const char *name;
      /* A staging folder of a running job counts as content, so two jobs never both take a folder as new. */
      while (d && (name = g_dir_read_name (d)))
        if (name[0] != '.' || g_str_has_prefix (name, BRO_STAGING_PREFIX))
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
          owned = FALSE;
          break;
        }
    }
  c->owns_output_dir = owned;
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
relative_to (const char *path, const char *base)
{
  g_autofree char *prefix = g_strconcat (base, G_DIR_SEPARATOR_S, NULL);
  return g_str_has_prefix (path, prefix) ? g_strdup (path + strlen (prefix)) : g_path_get_basename (path);
}

/* Moves file to the name rendered from tmpl. With an empty template the file keeps its name, or gets
 * fallback (split episodes, whose temporary names are meaningless). */
static char *
rename_to (Ctx *c, const char *file, GHashTable *v, const char *tmpl, const char *fallback, const char *out_dir)
{
  g_autofree char *t = g_strstrip (g_strdup (tmpl ? tmpl : ""));
  g_autofree char *rel = *t ? bro_template_render_path (t, v) : g_strdup ("");
  g_autofree char *target = NULL;
  g_autofree char *base = g_path_get_basename (file);
  char *unique;

  if (!*rel && fallback)
    {
      g_free (rel);
      rel = bro_sanitize_component (fallback);
    }
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

static void
add_episode (Ctx *c, const char *file, const char *out_dir, int episode, int source, int first, int last)
{
  EpisodeRec *e = g_new0 (EpisodeRec, 1);
  e->file = relative_to (file, out_dir);
  e->episode = episode;
  e->source = source;
  e->first = first;
  e->last = last;
  g_ptr_array_add (c->episodes, e);
}

static char *
rename_file (Ctx *c, const char *file, BroTitle *title, int ordinal, BroDiscInfo *info, const char *out_dir)
{
  const char *explicit_name = g_hash_table_lookup (c->req->name_overrides, GINT_TO_POINTER (title->index));
  const char *tmpl = explicit_name && *explicit_name ? explicit_name : c->req->drive->output.file_name_template;
  g_autoptr (GHashTable) v = base_values (c, BRO_JOB_RUNNING);
  g_autofree char *track = bro_track_label (title);
  int k = GPOINTER_TO_INT (g_hash_table_lookup (c->separate, GINT_TO_POINTER (title->index))) - 1;
  char *final;

  title_values (v, title, ordinal, info, file);
  bro_template_values_set (v, "track", track);
  if (k >= 0)
    {
      g_autofree char *label = bro_episode_label (c->first_episode + k, c->episode_width);
      g_autofree char *num = g_strdup_printf ("%d", c->first_episode + k);
      bro_template_values_set (v, "episode", label);
      bro_template_values_set (v, "episodeNumber", num);
    }
  final = rename_to (c, file, v, tmpl, NULL, out_dir);
  if (k >= 0)
    add_episode (c, final, out_dir, c->first_episode + k, bro_title_source_id (title) >= 0 ? bro_title_source_id (title) : title->index,
                 1, MAX (1, bro_title_chapters (title)));
  return final;
}

/* ---- episodes ---- */

/* A command-line tool next to sibling, on PATH, or in the usual install folders. */
static char *
find_tool (const char *name, const char *sibling)
{
  static const char *const dirs[] = { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", NULL };
  char *p;
  if (sibling)
    {
      g_autofree char *dir = g_path_get_dirname (sibling);
      p = g_build_filename (dir, name, NULL);
      if (g_file_test (p, G_FILE_TEST_IS_EXECUTABLE))
        return p;
      g_free (p);
    }
  if ((p = g_find_program_in_path (name)))
    return p;
  for (int i = 0; dirs[i]; i++)
    {
      p = g_build_filename (dirs[i], name, NULL);
      if (g_file_test (p, G_FILE_TEST_IS_EXECUTABLE))
        return p;
      g_free (p);
    }
  return NULL;
}

/* Mount point of a disc in a drive (from /proc/self/mounts). */
static char *
mount_point (const char *device)
{
  g_autofree char *text = NULL;
  g_auto (GStrv) lines = NULL;
  if (!device || !g_file_get_contents ("/proc/self/mounts", &text, NULL, NULL))
    return NULL;
  lines = g_strsplit (text, "\n", -1);
  for (char **l = lines; *l; l++)
    {
      g_auto (GStrv) f = g_strsplit (*l, " ", 3);
      if (f[0] && f[1] && g_str_equal (f[0], device))
        {
          GString *mp = g_string_new (f[1]);
          g_string_replace (mp, "\\040", " ", 0);
          return g_string_free (mp, FALSE);
        }
    }
  return NULL;
}

static BroVideoTS *
open_videots (Ctx *c, const BroSource *src)
{
  switch (src->kind)
    {
    case BRO_SOURCE_ISO:
      return bro_videots_open_iso (src->path);
    case BRO_SOURCE_FOLDER:
      return bro_videots_open_folder (src->path, c->res->disc_label);
    default:
      {
        g_autofree char *mp = mount_point (src->path);
        return mp ? bro_videots_open_folder (mp, c->res->disc_label) : NULL;
      }
    }
}

static gboolean
run_quiet (const char *const *argv, int timeout, GCancellable *cancel, GString *out)
{
  int status = -1;
  return bro_process_run (argv, NULL, NULL, timeout, cancel, out ? collect_line : NULL, out, &status, NULL, NULL, NULL) && status == 0;
}

/* Episode numbers read from the menu screens with ffmpeg + tesseract; NULL (with why) when the tools are missing. */
static GArray *
ocr_episode_numbers (Ctx *c, BroVideoTS *r, char **why)
{
  g_autofree char *ffmpeg = find_tool ("ffmpeg", NULL);
  g_autofree char *tesseract = find_tool ("tesseract", NULL);
  g_autofree char *dir = NULL;
  g_autoptr (GPtrArray) stills = NULL;
  GArray *numbers;
  if (!ffmpeg || !tesseract)
    {
      *why = g_strdup ("ffmpeg and tesseract are needed to read episode numbers from the menus");
      return NULL;
    }
  dir = g_dir_make_tmp ("bromelia-ocr-XXXXXX", NULL);
  numbers = g_array_new (FALSE, FALSE, sizeof (int));
  if (!dir)
    return numbers;
  stills = bro_dvd_menu_stills (r);
  for (guint i = 0; i < stills->len && !cancelled (c); i++)
    {
      g_autofree char *mpg_name = g_strdup_printf ("menu%u.mpg", i), *png_name = g_strdup_printf ("menu%u.png", i);
      g_autofree char *mpg = g_build_filename (dir, mpg_name, NULL), *png = g_build_filename (dir, png_name, NULL);
      gsize len;
      const char *data = g_bytes_get_data (stills->pdata[i], &len);
      GString *text = g_string_new (NULL);
      const char *ff[] = { ffmpeg, "-v", "quiet", "-y", "-f", "mpeg", "-i", mpg, "-frames:v", "1", "-vf", "scale=2160:1440,format=gray", png, NULL };
      const char *ts[] = { tesseract, png, "stdout", "--psm", "11", NULL };
      if (g_file_set_contents (mpg, data, len, NULL) && run_quiet (ff, 60, c->req->cancellable, NULL) && g_file_test (png, G_FILE_TEST_EXISTS))
        {
          g_autoptr (GArray) found = NULL;
          run_quiet (ts, 60, c->req->cancellable, text);
          found = bro_dvd_episode_numbers (text->str);
          for (guint k = 0; k < found->len; k++)
            {
              gboolean dup = FALSE;
              for (guint j = 0; j < numbers->len; j++)
                dup |= g_array_index (numbers, int, j) == g_array_index (found, int, k);
              if (!dup)
                g_array_append_val (numbers, g_array_index (found, int, k));
            }
        }
      g_string_free (text, TRUE);
      g_unlink (mpg);
      g_unlink (png);
    }
  g_rmdir (dir);
  return numbers;
}

static int
cmp_title_index (gconstpointer a, gconstpointer b)
{
  return (*(BroTitle *const *) a)->index - (*(BroTitle *const *) b)->index;
}

static int
cmp_int_values (gconstpointer a, gconstpointer b)
{
  return *(const int *) a - *(const int *) b;
}

static char *
numbers_text (GArray *n)
{
  GString *s = g_string_new ("[");
  g_autoptr (GArray) sorted = g_array_copy (n);
  g_array_sort (sorted, cmp_int_values);
  for (guint i = 0; i < sorted->len; i++)
    g_string_append_printf (s, "%s%d", i ? ", " : "", g_array_index (sorted, int, i));
  g_string_append_c (s, ']');
  return g_string_free (s, FALSE);
}

/* Decides, before ripping, which titles are episodes: a DVD "play all" title that will be split, or
 * separate episode-length titles. Also settles movie vs. TV and the first episode number. */
static void
prepare_episodes (Ctx *c, const BroSource *src, BroDiscInfo *info, GArray *indices)
{
  g_autoptr (GPtrArray) ripped = g_ptr_array_new ();
  g_autoptr (GPtrArray) plans = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_episode_plan_free);
  BroEpisodePlan *strict = NULL;
  int count, first = c->req->first_episode;
  g_autofree char *how = NULL;

  g_clear_pointer (&c->plan, bro_episode_plan_free);
  g_hash_table_remove_all (c->separate);
  for (guint i = 0; i < indices->len; i++)
    {
      BroTitle *t = bro_disc_info_title (info, g_array_index (indices, int, i));
      if (t)
        g_ptr_array_add (ripped, t);
    }
  if (c->identity && c->identity->format == BRO_FORMAT_DVD && c->req->drive->episodes.split_play_all)
    {
      phase (c, "Reading the disc menus");
      g_clear_pointer (&c->videots, bro_videots_free);
      c->videots = open_videots (c, src);
      if (c->videots)
        {
          g_autoptr (BroDvdAnalysis) a = bro_dvd_analyse (c->videots);
          g_autoptr (GPtrArray) all = a ? bro_dvd_plans (a) : NULL;
          for (guint i = 0; all && i < all->len; i++)
            {
              BroEpisodePlan *p = all->pdata[i];
              gboolean ripped_title = FALSE;
              for (guint k = 0; k < ripped->len; k++)
                ripped_title |= bro_title_source_id (ripped->pdata[k]) == p->title;
              if (!ripped_title)
                continue;
              GString *eps = g_string_new (NULL);
              for (guint k = 0; k < p->starts->len; k++)
                {
                  int ch = g_array_index (p->starts, int, k);
                  g_autofree char *at = bro_dvd_hms (g_array_index (p->chapter_starts, double, ch - 1));
                  g_string_append_printf (eps, "%s%d (%s, %s)", k ? ", " : "", ch, at, (char *) p->reasons->pdata[k]);
                }
              GString *tail = g_string_new (NULL);
              for (guint k = 0; k < p->tail->len; k++)
                g_string_append_printf (tail, "%s%d", k ? "," : "", g_array_index (p->tail, int, k));
              log_line (c, BRO_SEV_INFO, "Disc title %d: menus start %u episodes at chapters %s; last episode ends at chapter %d (%s)%s%s%s",
                        p->title, p->starts->len, eps->str, p->last_end, p->end_rule, tail->len ? "; chapter(s) " : "", tail->str,
                        tail->len ? " follow it" : "");
              g_string_free (eps, TRUE);
              g_string_free (tail, TRUE);
              g_ptr_array_add (plans, g_ptr_array_steal_index (all, i));
              i--;
            }
        }
      else
        log_line (c, BRO_SEV_WARNING, "Could not open the disc's VIDEO_TS to look for episodes");
    }
  for (guint i = 0; i < plans->len && !strict; i++)
    if (bro_episode_plan_plausible (plans->pdata[i], TRUE))
      strict = plans->pdata[i];
  resolve_identity (c, info, strict ? (int) strict->starts->len : 0, -1);
  if (c->identity->kind != BRO_KIND_TV)
    return;

  for (guint i = 0; i < plans->len && !c->plan; i++)
    {
      BroEpisodePlan *p = plans->pdata[i];
      BroTitle *t = NULL;
      if (!bro_episode_plan_plausible (p, FALSE))
        continue;
      for (guint k = 0; k < ripped->len && !t; k++)
        if (bro_title_source_id (ripped->pdata[k]) == p->title)
          t = ripped->pdata[k];
      if (t && bro_episode_plan_matches_chapters (p, bro_title_chapters (t)))
        c->plan = g_ptr_array_steal_index (plans, i);
      else
        log_line (c, BRO_SEV_WARNING, "MakeMKV's title %d has a different chapter count than the disc's navigation; not splitting it", p->title);
      break;
    }
  if (!c->plan && plans->len)
    {
      BroEpisodePlan *p = plans->pdata[0];
      GString *d = g_string_new (NULL);
      for (guint k = 0; k < p->episode_durations->len; k++)
        {
          g_autofree char *h = bro_dvd_hms (g_array_index (p->episode_durations, double, k));
          g_string_append_printf (d, "%s%s", k ? ", " : "", h);
        }
      log_line (c, BRO_SEV_INFO, "The menu jumps don't look like episodes (lengths %s); not splitting", d->str);
      g_string_free (d, TRUE);
    }
  if (!c->plan)
    {
      g_autoptr (GPtrArray) eps = bro_episode_like_titles (ripped);
      g_ptr_array_sort (eps, cmp_title_index);
      for (guint k = 0; k < eps->len; k++)
        g_hash_table_insert (c->separate, GINT_TO_POINTER (((BroTitle *) eps->pdata[k])->index), GINT_TO_POINTER (k + 1));
    }
  count = c->plan ? (int) c->plan->starts->len : (int) g_hash_table_size (c->separate);
  if (!count)
    return;
  how = g_strdup ("entered for this disc");
  if (first < 0 && c->req->drive->episodes.read_menu_numbers && c->videots)
    {
      char *why = NULL;
      g_autoptr (GArray) numbers = NULL;
      phase (c, "Reading episode numbers from the menus");
      numbers = ocr_episode_numbers (c, c->videots, &why);
      g_free (how);
      if (numbers && (first = bro_dvd_first_episode (numbers, count)) >= 0)
        {
          g_autofree char *n = numbers_text (numbers);
          how = g_strdup_printf ("menu text %s", n);
        }
      else if (why)
        how = why;
      else
        {
          g_autofree char *n = numbers_text (numbers);
          how = g_strdup_printf ("menus read %s, no clear numbering", n);
        }
    }
  if (first < 0)
    {
      char *h = g_strdup_printf ("numbered from 1 (%s)", how);
      g_free (how);
      how = h;
    }
  c->first_episode = first >= 0 ? first : 1;
  {
    g_autofree char *last = g_strdup_printf ("%d", c->first_episode + count - 1);
    c->episode_width = MAX (2, (int) strlen (last));
  }
  log_line (c, BRO_SEV_INFO, "Episodes %d–%d (%s)", c->first_episode, c->first_episode + count - 1, how);
}

static double
mkv_duration (Ctx *c, const char *file)
{
  const char *argv[] = { c->req->mkvmerge, "-J", file, NULL };
  GString *out = g_string_new (NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  double d = -1;
  if (run_quiet (argv, 120, NULL, out) && json_parser_load_from_data (parser, out->str, -1, NULL))
    {
      JsonObject *root = json_node_get_object (json_parser_get_root (parser));
      JsonObject *cont = root && json_object_has_member (root, "container") ? json_object_get_object_member (root, "container") : NULL;
      JsonObject *props = cont && json_object_has_member (cont, "properties") ? json_object_get_object_member (cont, "properties") : NULL;
      if (props && json_object_has_member (props, "duration"))
        d = json_object_get_double_member (props, "duration") / 1e9;
    }
  g_string_free (out, TRUE);
  return d;
}

static GArray *
mkv_chapter_starts (Ctx *c, const char *file)
{
  g_autofree char *mkvextract = find_tool ("mkvextract", c->req->mkvmerge);
  g_autofree char *tmp = NULL;
  g_autofree char *text = NULL;
  int fd;
  GArray *out = NULL;
  if (!mkvextract)
    return NULL;
  fd = g_file_open_tmp ("bromelia-chapters-XXXXXX.txt", &tmp, NULL);
  if (fd < 0)
    return NULL;
  close (fd);
  {
    const char *argv[] = { mkvextract, file, "chapters", "--simple", tmp, NULL };
    int status = -1;
    if (bro_process_run (argv, NULL, NULL, 120, NULL, NULL, NULL, &status, NULL, NULL, NULL) && status <= 1 &&
        g_file_get_contents (tmp, &text, NULL, NULL))
      out = bro_parse_simple_chapters (text);
  }
  g_unlink (tmp);
  return out;
}

static int
cmp_strings (gconstpointer a, gconstpointer b)
{
  return strcmp (*(char *const *) a, *(char *const *) b);
}

/* Splits the ripped "play all" title into episode files named with the file name template. */
static gboolean
split_episodes (Ctx *c, const char *out_dir, BroDiscInfo *info, GError **error)
{
  BroEpisodePlan *p = c->plan;
  GHashTableIter it;
  gpointer k, v;
  const char *file = NULL;
  BroTitle *title = NULL;
  g_autofree char *base = NULL, *dir = NULL, *uuid = NULL, *prefix = NULL, *pattern = NULL, *stem = NULL;
  g_autoptr (GArray) starts = NULL, split = NULL, mapped = NULL;
  g_autoptr (GPtrArray) parts = NULL, outputs = NULL;
  GString *list;
  int status = -1;
  double dur;
  guint at;

  if (!p)
    return TRUE;
  g_hash_table_iter_init (&it, c->file_titles);
  while (g_hash_table_iter_next (&it, &k, &v))
    {
      BroTitle *t = bro_disc_info_title (info, GPOINTER_TO_INT (v) - 1);
      if (t && bro_title_source_id (t) == p->title)
        {
          file = k;
          title = t;
        }
    }
  if (!file)
    return TRUE;
  base = g_path_get_basename (file);
  if (!c->req->mkvmerge)
    {
      log_line (c, BRO_SEV_WARNING, "Splitting episodes needs mkvmerge (MKVToolNix); kept %s as one file", base);
      return TRUE;
    }
  phase (c, "Splitting episodes");
  dur = mkv_duration (c, file);
  if (dur >= 0 && ABS (dur - p->duration) > 2)
    {
      g_autofree char *a = bro_dvd_hms (dur), *b = bro_dvd_hms (p->duration);
      log_line (c, BRO_SEV_WARNING, "%s lasts %s, the disc title %s; not splitting", base, a, b);
      return TRUE;
    }
  starts = mkv_chapter_starts (c, file);
  split = bro_episode_plan_split_chapters (p);
  mapped = bro_dvd_mkv_chapters (split, p, starts);
  if (!mapped)
    {
      log_line (c, BRO_SEV_WARNING, "The chapters of %s don't line up with the disc's episodes; not splitting", base);
      return TRUE;
    }
  dir = g_path_get_dirname (file);
  uuid = g_uuid_string_random ();
  prefix = g_strdup_printf (".bromelia-split-%.8s", uuid);
  {
    g_autofree char *name = g_strdup_printf ("%s-%%03d.mkv", prefix);
    pattern = g_build_filename (dir, name, NULL);
  }
  list = g_string_new ("chapters:");
  for (guint i = 0; i < mapped->len; i++)
    g_string_append_printf (list, "%s%d", i ? "," : "", g_array_index (mapped, int, i));
  {
    const char *argv[] = { c->req->mkvmerge, "-o", pattern, "--split", list->str, file, NULL };
    g_autofree char *cmd = bro_command_line (argv);
    log_line (c, BRO_SEV_INFO, "$ %s", cmd);
    bro_process_run (argv, NULL, NULL, 0, c->req->cancellable, NULL, NULL, &status, NULL, NULL, NULL);
  }
  g_string_free (list, TRUE);
  parts = g_ptr_array_new_with_free_func (g_free);
  {
    g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name (d)))
      if (g_str_has_prefix (name, prefix))
        g_ptr_array_add (parts, g_build_filename (dir, name, NULL));
  }
  g_ptr_array_sort (parts, cmp_strings);
  if (status < 0 || status > 1 || parts->len != mapped->len + 1)
    {
      for (guint i = 0; i < parts->len; i++)
        g_unlink (parts->pdata[i]);
      if (cancelled (c))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          return FALSE;
        }
      log_line (c, BRO_SEV_WARNING, "mkvmerge could not split %s; kept it as one file", base);
      return TRUE;
    }
  stem = g_strdup (base);
  if (strrchr (stem, '.')) *strrchr (stem, '.') = '\0';
  outputs = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < parts->len; i++)
    {
      g_autoptr (GHashTable) vals = base_values (c, BRO_JOB_RUNNING);
      g_autofree char *track = bro_track_label (title);
      g_autofree char *range = NULL, *full_track = NULL, *fallback = NULL;
      int f, l;
      title_values (vals, title, (int) i + 1, info, parts->pdata[i]);
      if (i < p->starts->len)
        {
          g_autofree char *label = bro_episode_label (c->first_episode + (int) i, c->episode_width);
          g_autofree char *num = g_strdup_printf ("%d", c->first_episode + (int) i);
          bro_episode_plan_range (p, i, &f, &l);
          bro_template_values_set (vals, "episode", label);
          bro_template_values_set (vals, "episodeNumber", num);
          fallback = g_strdup_printf ("%s - Episode %d", stem, c->first_episode + (int) i);
        }
      else
        {
          f = g_array_index (p->tail, int, 0);
          l = g_array_index (p->tail, int, p->tail->len - 1);
          fallback = g_strdup_printf ("%s - after last episode", stem);
        }
      range = f == l ? g_strdup_printf ("%d", f) : g_strdup_printf ("%d-%d", f, l);
      full_track = g_strdup_printf ("%s Ch %s", track, range);
      bro_template_values_set (vals, "track", full_track);
      char *final = rename_to (c, parts->pdata[i], vals, c->req->drive->output.file_name_template, fallback, out_dir);
      if (i < p->starts->len)
        add_episode (c, final, out_dir, c->first_episode + (int) i, p->title, f, l);
      g_ptr_array_add (outputs, final);
    }
  log_line (c, BRO_SEV_INFO, "Split %s into %u episode(s)%s", base, p->starts->len, parts->len > p->starts->len ? " and a closing clip" : "");
  for (at = 0; at < c->res->files->len && !g_str_equal (c->res->files->pdata[at], file); at++)
    ;
  if (!c->req->drive->episodes.keep_play_all)
    {
      if (at < c->res->files->len)
        g_ptr_array_remove_index (c->res->files, at);
      g_unlink (file);
      log_line (c, BRO_SEV_INFO, "Removed the unsplit title %s", base);
    }
  else if (at < c->res->files->len)
    at++;
  for (guint i = 0; i < outputs->len; i++)
    {
      g_ptr_array_insert (c->res->files, (gint) MIN (at + i, c->res->files->len), g_strdup (outputs->pdata[i]));
      update (c, BRO_UPDATE_FILE, BRO_SEV_INFO, outputs->pdata[i], 0, 0);
    }
  return TRUE;
}

/* ---- archive ---- */

typedef struct {
  Ctx *c;
  gint64 before, total, size, last_update;
} HashProgress;

static gboolean
on_hash_progress (gint64 done, gpointer data)
{
  HashProgress *h = data;
  gint64 now = g_get_monotonic_time ();
  if (now - h->last_update > 100000)
    {
      h->last_update = now;
      update (h->c, BRO_UPDATE_PROGRESS, BRO_SEV_INFO, NULL, (double) done / MAX (h->size, 1), (double) (h->before + done) / MAX (h->total, 1));
    }
  return !cancelled (h->c);
}

static void write_archive_record (Ctx *c, const char *out_dir, BroJobState status);

/* SHA-256 of every produced file (folders are walked), paths relative to base. NULL (with error) on failure. */
static GPtrArray *
hash_produced_files (Ctx *c, const char *base, GError **error)
{
  g_autoptr (GPtrArray) files = bro_checksum_list_files (c->res->files, base);
  gint64 total = 0, done = 0;
  for (guint i = 0; i < files->len; i++)
    total += ((BroChecksum *) files->pdata[i])->size;
  phase (c, "Computing checksums");
  steps (c, c->step_count, c->step_count);
  for (guint i = 0; i < files->len; i++)
    {
      BroChecksum *e = files->pdata[i];
      g_autofree char *full = g_build_filename (base, e->path, NULL);
      HashProgress h = { c, done, total, e->size, 0 };
      g_autoptr (GError) err = NULL;
      update (c, BRO_UPDATE_OPERATION, BRO_SEV_INFO, e->path, 0, 0);
      e->sha256 = bro_sha256_file (full, on_hash_progress, &h, &err);
      if (!e->sha256)
        {
          if (cancelled (c))
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          else
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read %s to compute its checksum: %s", e->path,
                         err ? err->message : "unknown error");
          return NULL;
        }
      done += e->size;
    }
  update (c, BRO_UPDATE_OPERATION, BRO_SEV_INFO, "", 0, 0);
  return g_steal_pointer (&files);
}

/* Writes SHA256SUMS and bromelia.json into dir. Returns an error message when SHA256SUMS can't be written. */
static char *
write_archive (Ctx *c, GPtrArray *entries, const char *dir, BroJobState status)
{
  const BroArchiveConfig *cfg = &c->req->drive->archive;
  if (c->checksums)
    g_ptr_array_unref (c->checksums);
  c->checksums = g_ptr_array_ref (entries);
  if (cfg->checksums && entries->len)
    {
      g_autoptr (GError) err = NULL;
      char *path = bro_checksums_write_merged (dir, entries, &err);
      if (!path)
        {
          char *msg = g_strdup_printf ("Could not write %s: %s", BRO_CHECKSUM_FILE, err ? err->message : "");
          log_line (c, BRO_SEV_ERROR, "%s", msg);
          return msg;
        }
      g_free (c->checksum_file);
      c->checksum_file = path;
      log_line (c, BRO_SEV_INFO, "Wrote SHA-256 checksums of %u file(s) to %s", entries->len, BRO_CHECKSUM_FILE);
    }
  if (cfg->archive_record)
    write_archive_record (c, dir, status);
  return NULL;
}

/* Names of the entries of dir that aren't hidden, sorted. */
static GPtrArray *
visible_items (const char *dir)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *n;
  while (d && (n = g_dir_read_name (d)))
    if (n[0] != '.')
      g_ptr_array_add (names, g_strdup (n));
  g_ptr_array_sort (names, cmp_strings);
  return names;
}

static void
remove_tree (const char *path)
{
  if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    {
      g_autoptr (GDir) d = g_dir_open (path, 0, NULL);
      const char *n;
      while (d && (n = g_dir_read_name (d)))
        {
          g_autofree char *child = g_build_filename (path, n, NULL);
          remove_tree (child);
        }
      g_rmdir (path);
    }
  else
    g_unlink (path);
}

/* Where the files of a job that didn't succeed go. When the output folder was created for this job it is renamed
 * ("Name [INCOMPLETE]"); in a shared folder a subfolder is used ("INCOMPLETE - 1a2b3c4d"). Sets dest and the
 * staging folder's (possibly new) location. */
static void
marked_folder (Ctx *c, const char *tag, char **dest, char **from)
{
  g_autofree char *short_id = g_ascii_strdown (c->req->job_id, MIN (strlen (c->req->job_id), 8));
  g_autoptr (GPtrArray) items = visible_items (c->output_dir);
  if (c->owns_output_dir && items->len == 0)
    {
      g_autofree char *parent = g_path_get_dirname (c->output_dir);
      g_autofree char *base = g_path_get_basename (c->output_dir);
      g_autofree char *name = g_strdup_printf ("%s [%s]", base, tag);
      g_autofree char *want = g_build_filename (parent, name, NULL);
      char *target = bro_unique_path (want);
      if (g_rename (c->output_dir, target) == 0)
        {
          g_autofree char *stage = g_path_get_basename (c->work_dir);
          g_free (c->output_dir);
          c->output_dir = g_strdup (target);
          *from = g_build_filename (target, stage, NULL);
          *dest = target;
          return;
        }
      log_line (c, BRO_SEV_WARNING, "Could not rename %s", c->output_dir);
      g_free (target);
    }
  {
    g_autofree char *name = g_strdup_printf ("%s - %s", tag, short_id);
    g_autofree char *want = g_build_filename (c->output_dir, name, NULL);
    *dest = bro_unique_path (want);
    g_mkdir_with_parents (*dest, 0755);
    *from = g_strdup (c->work_dir);
  }
}

/* Explains in the folder itself why its files are not a finished archive. */
static void
write_note (Ctx *c, const char *dir, const char *tag, BroJobState status)
{
  g_autoptr (GString) t = g_string_new (NULL);
  g_autofree char *name = g_strconcat (tag, ".txt", NULL);
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autofree char *log = g_build_filename (c->req->job_dir, "log.txt", NULL);
  g_string_append_printf (t, "Bromelia job %s: %s.\n", c->req->job_id, bro_job_state_label (status));
  g_string_append (t, "These files are NOT a finished archive. Rip the disc again (clean it first if it has read errors),\n");
  g_string_append (t, "or check the files yourself before using them.\n\n");
  if (c->res->error)
    g_string_append_printf (t, "%s\n\n", c->res->error);
  if (c->data_errors->len)
    {
      g_string_append (t, "Errors reported by MakeMKV while reading the disc:\n");
      for (guint i = 0; i < c->data_errors->len; i++)
        g_string_append_printf (t, "  %s\n", (char *) c->data_errors->pdata[i]);
      g_string_append (t, "\n");
    }
  g_string_append_printf (t, "Full log: %s\n", log);
  g_file_set_contents (path, t->str, -1, NULL);
}

/* A path under the staging folder, moved to where its top-level item went (moved: name -> new path). */
static char *
relocate (const char *path, const char *stage, GHashTable *moved)
{
  g_autofree char *prefix = g_strconcat (stage, G_DIR_SEPARATOR_S, NULL);
  const char *rest, *slash;
  g_autofree char *first = NULL;
  const char *top;
  if (!g_str_has_prefix (path, prefix))
    return g_strdup (path);
  rest = path + strlen (prefix);
  slash = strchr (rest, G_DIR_SEPARATOR);
  first = slash ? g_strndup (rest, slash - rest) : g_strdup (rest);
  top = g_hash_table_lookup (moved, first);
  if (!top)
    return g_strdup (path);
  return slash ? g_build_filename (top, slash + 1, NULL) : g_strdup (top);
}

/* Moves the job's files out of the staging folder. A job that succeeded goes into the output folder. Anything else
 * goes into a folder marked "[INCOMPLETE]" (failed or cancelled) or "[READ ERRORS]", with a note explaining why, so
 * an unfinished or damaged rip can never look like a finished archive. Checksums and the archive record are written
 * for complete files, including files with read errors. */
static BroJobState
finish_output (Ctx *c, BroJobState status)
{
  const BroArchiveConfig *cfg = &c->req->drive->archive;
  g_autoptr (GPtrArray) staged = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  g_autoptr (GPtrArray) items = NULL;
  g_autoptr (GHashTable) moved = NULL;
  g_autofree char *dest = NULL, *from = NULL;
  const char *tag;
  gboolean complete, left = FALSE;

  if (!c->work_dir || !c->output_dir)
    return status;
  staged = visible_items (c->work_dir);
  if (staged->len == 0)
    {
      remove_tree (c->work_dir);
      /* Remove the folder reserved for this job when nothing was saved. */
      if (status != BRO_JOB_SUCCEEDED && c->owns_output_dir)
        {
          g_autoptr (GDir) d = g_dir_open (c->output_dir, 0, NULL);
          const char *n;
          gboolean empty = d != NULL;
          while (d && (n = g_dir_read_name (d)))
            empty &= g_str_equal (n, ".directory");
          if (empty)
            {
              remove_tree (c->output_dir);
              g_clear_pointer (&c->res->output_dir, g_free);
              update (c, BRO_UPDATE_OUTPUT_DIR, BRO_SEV_INFO, "", 0, 0);
            }
        }
      return status;
    }

  /* Hash while the files are still in the staging folder: if a file can't be read back, it stays out of the archive. */
  complete = status == BRO_JOB_SUCCEEDED || status == BRO_JOB_COMPLETED_WITH_ERRORS;
  if (complete && (cfg->checksums || cfg->archive_record) && c->res->files->len)
    {
      g_autoptr (GError) err = NULL;
      entries = hash_produced_files (c, c->work_dir, &err);
      if (!entries)
        {
          g_free (c->res->error);
          if (cancelled (c))
            {
              status = BRO_JOB_CANCELLED;
              c->res->error = g_strdup ("Cancelled by user");
              log_line (c, BRO_SEV_WARNING, "Job cancelled");
            }
          else
            {
              status = BRO_JOB_FAILED;
              c->res->error = g_strdup (err ? err->message : "Could not compute checksums");
              log_line (c, BRO_SEV_ERROR, "%s", c->res->error);
            }
        }
    }

  tag = status == BRO_JOB_SUCCEEDED ? NULL : status == BRO_JOB_COMPLETED_WITH_ERRORS ? "READ ERRORS" : "INCOMPLETE";
  if (tag)
    marked_folder (c, tag, &dest, &from);
  else
    {
      dest = g_strdup (c->output_dir);
      from = g_strdup (c->work_dir);
    }

  phase (c, "Moving files");
  moved = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  items = visible_items (from);
  for (guint i = 0; i < items->len; i++)
    {
      const char *name = items->pdata[i];
      g_autofree char *source = g_build_filename (from, name, NULL);
      g_autofree char *want = g_build_filename (dest, name, NULL);
      char *target = bro_unique_path (want);
      if (g_rename (source, target) == 0)
        g_hash_table_insert (moved, g_strdup (name), target);
      else
        {
          left = TRUE;
          log_line (c, BRO_SEV_ERROR, "Could not move %s out of %s", name, from);
          g_free (target);
        }
    }
  if (left)
    {
      if (status == BRO_JOB_SUCCEEDED)
        {
          status = BRO_JOB_FAILED;
          g_free (c->res->error);
          c->res->error = g_strdup_printf ("Some files could not be moved into the output folder; they are still in %s", from);
        }
    }
  else
    remove_tree (from); /* only temporary (hidden) files are left */

  /* Paths recorded while the files were in the staging folder now point to their final place. */
  for (guint i = 0; i < c->res->files->len; i++)
    {
      char *p = relocate (c->res->files->pdata[i], c->work_dir, moved);
      g_free (c->res->files->pdata[i]);
      c->res->files->pdata[i] = p;
    }
  {
    GHashTable *titles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, c->file_titles);
    while (g_hash_table_iter_next (&it, &k, &v))
      g_hash_table_insert (titles, relocate (k, c->work_dir, moved), v);
    g_hash_table_unref (c->file_titles);
    c->file_titles = titles;
  }
  for (guint i = 0; i < c->episodes->len; i++)
    {
      EpisodeRec *e = c->episodes->pdata[i];
      g_autofree char *old = g_build_filename (c->work_dir, e->file, NULL);
      g_autofree char *now = relocate (old, c->work_dir, moved);
      g_free (e->file);
      e->file = relative_to (now, dest);
    }
  for (guint i = 0; entries && i < entries->len; i++)
    {
      BroChecksum *e = entries->pdata[i];
      g_autofree char *old = g_build_filename (c->work_dir, e->path, NULL);
      g_autofree char *now = relocate (old, c->work_dir, moved);
      g_free (e->path);
      e->path = relative_to (now, dest);
    }
  g_free (c->res->output_dir);
  c->res->output_dir = g_strdup (dest);
  update (c, BRO_UPDATE_OUTPUT_DIR, BRO_SEV_INFO, dest, 0, 0);
  if (tag)
    log_line (c, BRO_SEV_WARNING, "Kept %u item(s) apart in %s", g_hash_table_size (moved), dest);
  else
    log_line (c, BRO_SEV_INFO, "Moved %u item(s) into %s", g_hash_table_size (moved), dest);

  if ((status == BRO_JOB_SUCCEEDED || status == BRO_JOB_COMPLETED_WITH_ERRORS) && entries)
    {
      g_autofree char *err = write_archive (c, entries, dest, status);
      if (err && status == BRO_JOB_SUCCEEDED)
        {
          status = BRO_JOB_FAILED;
          g_free (c->res->error);
          c->res->error = g_steal_pointer (&err);
        }
    }
  if (status != BRO_JOB_SUCCEEDED)
    write_note (c, dest, tag ? tag : "INCOMPLETE", status);
  return status;
}

static void
add_optional_int (JsonBuilder *b, const char *name, int v)
{
  if (v < 0)
    return;
  json_builder_set_member_name (b, name);
  json_builder_add_int_value (b, v);
}

static void
build_episodes (JsonBuilder *b, Ctx *c)
{
  json_builder_set_member_name (b, "episodes");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->episodes->len; i++)
    {
      EpisodeRec *e = c->episodes->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "file");
      json_builder_add_string_value (b, e->file);
      json_builder_set_member_name (b, "episode");
      json_builder_add_int_value (b, e->episode);
      json_builder_set_member_name (b, "sourceTitleId");
      json_builder_add_int_value (b, e->source);
      json_builder_set_member_name (b, "firstChapter");
      json_builder_add_int_value (b, e->first);
      json_builder_set_member_name (b, "lastChapter");
      json_builder_add_int_value (b, e->last);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
}

static void
build_checksums (JsonBuilder *b, const char *name, Ctx *c)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_array (b);
  for (guint i = 0; c->checksums && i < c->checksums->len; i++)
    {
      BroChecksum *e = c->checksums->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "path");
      json_builder_add_string_value (b, e->path);
      json_builder_set_member_name (b, "size");
      json_builder_add_int_value (b, e->size);
      json_builder_set_member_name (b, "sha256");
      json_builder_add_string_value (b, e->sha256 ? e->sha256 : "");
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
}

static void
write_archive_record (Ctx *c, const char *out_dir, BroJobState status)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *text = NULL, *target = NULL, *path = g_build_filename (out_dir, "bromelia.json", NULL);
  g_autofree char *src = bro_source_info_argument (c->req->source);
  g_autofree char *code = NULL;
  BroIdentity *id = c->identity;
  BroDiscInfo *info = c->res->info;
  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
  g_autofree char *finished = g_date_time_format_iso8601 (now);
  g_autoptr (GDateTime) st = g_date_time_new_from_unix_utc (c->res->started_at);
  g_autofree char *started = g_date_time_format_iso8601 (st);
  if (!id)
    return;
  code = bro_identity_format_code (id);
#define S(k, v) do { json_builder_set_member_name (b, k); json_builder_add_string_value (b, (v) ? (v) : ""); } while (0)
  json_builder_begin_object (b);
  S ("format", "bromelia-archive");
  json_builder_set_member_name (b, "version");
  json_builder_add_int_value (b, 2);
  S ("status", bro_job_state_word (status));
  S ("name", id->name);
  S ("kind", bro_kind_token (id->kind));
  json_builder_set_member_name (b, "disc");
  json_builder_begin_object (b);
  S ("label", c->res->disc_label);
  S ("volumeName", info ? bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME) : "");
  S ("type", info ? bro_disc_info_attr (info, BRO_ATTR_TYPE) : "");
  S ("format", bro_format_token (id->format));
  S ("formatCode", code);
  json_builder_set_member_name (b, "encrypted");
  json_builder_add_boolean_value (b, id->encrypted);
  add_optional_int (b, "season", id->label.season);
  add_optional_int (b, "part", id->label.part);
  add_optional_int (b, "volume", id->label.volume);
  add_optional_int (b, "disc", id->label.disc);
  json_builder_end_object (b);
  S ("rip", bro_rip_mode_makes_mkv (c->req->mode) ? "Rip" : "Backup");
  S ("mode", bro_rip_mode_to_string (c->req->mode));
  S ("source", src);
  S ("driveName", c->req->drive->name);
  S ("makemkv", c->makemkv_version);
  S ("jobId", c->req->job_id);
  S ("startedAt", started);
  S ("finishedAt", finished);
  json_builder_set_member_name (b, "titles");
  json_builder_begin_array (b);
  for (guint i = 0; c->rip_titles && info && i < c->rip_titles->len; i++)
    {
      BroTitle *t = bro_disc_info_title (info, g_array_index (c->rip_titles, int, i));
      if (!t)
        continue;
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "index");
      json_builder_add_int_value (b, t->index);
      add_optional_int (b, "sourceTitleId", bro_title_source_id (t));
      S ("sourceFile", bro_title_str (t, BRO_ATTR_SOURCE_FILE_NAME));
      S ("duration", bro_title_str (t, BRO_ATTR_DURATION));
      json_builder_set_member_name (b, "chapters");
      json_builder_add_int_value (b, bro_title_chapters (t));
      json_builder_set_member_name (b, "sizeBytes");
      json_builder_add_int_value (b, bro_title_size (t));
      S ("segmentMap", bro_title_str (t, BRO_ATTR_SEGMENTS_MAP));
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  build_episodes (b, c);
  build_checksums (b, "files", c);
  json_builder_set_member_name (b, "warnings");
  json_builder_add_int_value (b, c->res->warnings);
  json_builder_set_member_name (b, "errors");
  json_builder_add_int_value (b, c->res->errors);
  json_builder_set_member_name (b, "errorMessages");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->error_messages->len; i++)
    json_builder_add_string_value (b, c->error_messages->pdata[i]);
  json_builder_end_array (b);
  json_builder_set_member_name (b, "readErrors");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->data_errors->len; i++)
    json_builder_add_string_value (b, c->data_errors->pdata[i]);
  json_builder_end_array (b);
  json_builder_end_object (b);
#undef S
  root = json_builder_get_root (b);
  text = bro_json_to_string (root, TRUE);
  target = bro_unique_path (path);
  if (g_file_set_contents (target, text, -1, NULL))
    {
      g_autofree char *name = g_path_get_basename (target);
      log_line (c, BRO_SEV_INFO, "Wrote the archive record %s", name);
    }
  else
    log_line (c, BRO_SEV_WARNING, "Could not write %s", target);
}

/* File / folder name of a backup from the file name template (rip = Backup, no episode or track). */
static char *
backup_name (Ctx *c, gboolean decrypt)
{
  g_autofree char *tmpl = g_strstrip (g_strdup (c->req->drive->output.file_name_template));
  g_autoptr (GHashTable) v = NULL;
  static const char *const blank[] = { "title", "index", "n", "source", "duration", "chapters", "original", "comment", NULL };
  char *rel;
  GString *out;
  if (!*tmpl)
    return g_strdup ("");
  v = base_values (c, BRO_JOB_RUNNING);
  bro_template_values_set (v, "rip", "Backup");
  if (c->identity)
    {
      g_autofree char *code = bro_format_code (c->identity->format, !decrypt);
      bro_template_values_set (v, "format", code);
    }
  for (int i = 0; blank[i]; i++)
    bro_template_values_set (v, blank[i], "");
  rel = bro_template_render_path (tmpl, v);
  out = g_string_new (rel);
  g_free (rel);
  g_string_replace (out, "/", " - ", 0);
  return g_string_free (out, FALSE);
}

/* ---- checks against the disc listing ---- */

void
bro_mkv_probe_free (BroMkvProbe *p)
{
  if (!p)
    return;
  g_ptr_array_unref (p->track_types);
  g_free (p);
}

BroMkvProbe *
bro_mkv_probe_parse (const char *json)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root, *container, *props;
  BroMkvProbe *p;
  if (!json || !json_parser_load_from_data (parser, json, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return NULL;
  root = json_node_get_object (json_parser_get_root (parser));
  container = json_object_has_member (root, "container") ? json_object_get_object_member (root, "container") : NULL;
  if (!container)
    return NULL;
  if (json_object_has_member (container, "recognized") && !json_object_get_boolean_member (container, "recognized"))
    return NULL;
  p = g_new0 (BroMkvProbe, 1);
  p->track_types = g_ptr_array_new_with_free_func (g_free);
  props = json_object_has_member (container, "properties") ? json_object_get_object_member (container, "properties") : NULL;
  if (props && json_object_has_member (props, "duration"))
    {
      JsonNode *d = json_object_get_member (props, "duration");
      if (JSON_NODE_HOLDS_VALUE (d))
        {
          GType t = json_node_get_value_type (d);
          p->has_duration = TRUE;
          p->duration = (t == G_TYPE_DOUBLE ? json_node_get_double (d) : (double) json_node_get_int (d)) / 1e9;
        }
    }
  if (json_object_has_member (root, "tracks"))
    {
      JsonArray *tracks = json_object_get_array_member (root, "tracks");
      for (guint i = 0; tracks && i < json_array_get_length (tracks); i++)
        {
          JsonObject *t = json_array_get_object_element (tracks, i);
          g_ptr_array_add (p->track_types, g_strdup (t && json_object_has_member (t, "type") ? json_object_get_string_member (t, "type") : ""));
        }
    }
  if (json_object_has_member (root, "chapters"))
    {
      JsonArray *ch = json_object_get_array_member (root, "chapters");
      for (guint i = 0; ch && i < json_array_get_length (ch); i++)
        {
          JsonObject *e = json_array_get_object_element (ch, i);
          if (e && json_object_has_member (e, "num_entries"))
            p->chapters += (int) json_object_get_int_member (e, "num_entries");
        }
    }
  return p;
}

double
bro_rip_duration_tolerance (double expected)
{
  /* 5 s or 0.5 %, whichever is larger. */
  return MAX (5.0, expected * 0.005);
}

static char *
hms (int s)
{
  return g_strdup_printf ("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
}

void
bro_rip_check (const BroMkvProbe *p, BroTitle *title, GPtrArray *problems, GPtrArray *notes)
{
  gboolean has_video = FALSE, listed_video = FALSE;
  int expected = bro_title_duration (title), chapters = bro_title_chapters (title);
  for (guint i = 0; i < p->track_types->len; i++)
    has_video |= g_str_equal (p->track_types->pdata[i], "video");
  for (guint i = 0; i < title->tracks->len; i++)
    listed_video |= bro_track_kind (title->tracks->pdata[i]) == BRO_TRACK_VIDEO;
  if (p->track_types->len == 0)
    g_ptr_array_add (problems, g_strdup ("the file contains no tracks"));
  else if (!has_video && listed_video)
    g_ptr_array_add (problems, g_strdup ("the file contains no video track"));
  if (expected > 0)
    {
      if (!p->has_duration)
        g_ptr_array_add (problems, g_strdup ("mkvmerge reports no duration"));
      else if (ABS (p->duration - expected) > bro_rip_duration_tolerance (expected))
        {
          g_autofree char *a = hms ((int) (p->duration + 0.5)), *b = hms (expected);
          g_ptr_array_add (problems, g_strdup_printf ("it lasts %s, the disc listing says %s", a, b));
        }
    }
  if (title->tracks->len > 0 && p->track_types->len > title->tracks->len)
    g_ptr_array_add (notes, g_strdup_printf ("%u tracks, the disc listing has %u", p->track_types->len, title->tracks->len));
  /* MakeMKV may add a chapter at 00:00 (profile option), so allow one extra. */
  if (chapters > 1 && (p->chapters < chapters || p->chapters > chapters + 1))
    g_ptr_array_add (notes, g_strdup_printf ("%d chapters, the disc listing has %d", p->chapters, chapters));
}

static char *
find_entry (const char *dir, const char *name)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *n;
  while (d && (n = g_dir_read_name (d)))
    if (g_ascii_strcasecmp (n, name) == 0)
      return g_build_filename (dir, n, NULL);
  return NULL;
}

char *
bro_backup_problem (const char *path, gboolean iso)
{
  g_autofree char *name = g_path_get_basename (path);
  if (iso)
    {
      FILE *f;
      char id[6] = { 0 };
      gboolean ok;
      if (g_file_test (path, G_FILE_TEST_IS_DIR))
        return g_strdup_printf ("%s is a folder, not an ISO image", name);
      if (!(f = fopen (path, "rb")))
        return g_strdup_printf ("%s can't be read", name);
      ok = fseek (f, 32769, SEEK_SET) == 0 && fread (id, 1, 5, f) == 5 && (strcmp (id, "CD001") == 0 || strcmp (id, "BEA01") == 0);
      fclose (f);
      return ok ? NULL : g_strdup_printf ("%s is not an ISO / UDF image", name);
    }
  if (!g_file_test (path, G_FILE_TEST_IS_DIR))
    return g_file_test (path, G_FILE_TEST_EXISTS) ? g_strdup_printf ("%s is not a folder", name) : g_strdup_printf ("%s was not created", name);
  {
    g_autofree char *bdmv = find_entry (path, "BDMV");
    g_autofree char *vts = bdmv ? NULL : find_entry (path, "VIDEO_TS");
    g_autofree char *hd = bdmv || vts ? NULL : find_entry (path, "HVDVD_TS");
    if (bdmv)
      {
        g_autofree char *index = find_entry (bdmv, "index.bdmv");
        return index ? NULL : g_strdup ("BDMV/index.bdmv is missing");
      }
    if (vts)
      {
        g_autofree char *ifo = find_entry (vts, "VIDEO_TS.IFO");
        return ifo ? NULL : g_strdup ("VIDEO_TS/VIDEO_TS.IFO is missing");
      }
    return hd ? NULL : g_strdup ("it contains no BDMV, VIDEO_TS or HVDVD_TS folder");
  }
}

static char *
title_key (BroTitle *t)
{
  return g_strdup_printf ("%d|%d|%s", bro_title_source_id (t), bro_title_duration (t), bro_title_str (t, BRO_ATTR_SEGMENTS_MAP));
}

char *
bro_listing_different_disc (BroDiscInfo *old, BroDiscInfo *now)
{
  const char *a = bro_disc_info_attr (old, BRO_ATTR_VOLUME_NAME), *b = bro_disc_info_attr (now, BRO_ATTR_VOLUME_NAME);
  g_autoptr (GHashTable) keys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gboolean common = FALSE;
  if (a && *a && b && *b && !g_str_equal (a, b))
    return g_strdup_printf ("the disc is now “%s”, it was “%s” when it was opened", b, a);
  for (guint i = 0; i < old->titles->len; i++)
    g_hash_table_add (keys, title_key (old->titles->pdata[i]));
  for (guint i = 0; i < now->titles->len && !common; i++)
    {
      g_autofree char *k = title_key (now->titles->pdata[i]);
      common = g_hash_table_contains (keys, k);
    }
  if (old->titles->len && now->titles->len && !common)
    return g_strdup ("none of its titles match the titles listed when it was opened");
  return NULL;
}

static int
cmp_int (gconstpointer a, gconstpointer b)
{
  return *(const int *) a - *(const int *) b;
}

GHashTable *
bro_listing_map (GArray *indices, BroDiscInfo *old, BroDiscInfo *now, GHashTable *same_tracks, GError **error)
{
  g_autoptr (GHashTable) map = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_autoptr (GHashTable) used = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_autoptr (GArray) sorted = g_array_copy (indices);
  g_array_sort (sorted, cmp_int);
  for (guint n = 0; n < sorted->len; n++)
    {
      int i = g_array_index (sorted, int, n);
      BroTitle *t = bro_disc_info_title (old, i), *match = NULL, *first = NULL;
      g_autofree char *k = NULL;
      if (g_hash_table_contains (map, GINT_TO_POINTER (i)))
        continue;
      if (!t)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Title %d is not in the disc listing", i);
          return NULL;
        }
      k = title_key (t);
      for (guint j = 0; j < now->titles->len && !match; j++)
        {
          BroTitle *x = now->titles->pdata[j];
          g_autofree char *k2 = title_key (x);
          if (!g_str_equal (k, k2) || g_hash_table_contains (used, GINT_TO_POINTER (x->index)))
            continue;
          if (!first)
            first = x;
          if (x->index == i)
            match = x;
        }
      if (!match)
        match = first;
      if (!match)
        {
          const char *file = bro_title_str (t, BRO_ATTR_SOURCE_FILE_NAME);
          g_autofree char *where = *file ? g_strdup (file) : g_strdup_printf ("source %d", bro_title_source_id (t));
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Title %d (%s, %s) is not in the new disc listing. The disc may have been changed, or the minimum title "
                       "length differs. Open the disc again and choose the titles.", i, bro_title_str (t, BRO_ATTR_DURATION), where);
          return NULL;
        }
      if (same_tracks && g_hash_table_contains (same_tracks, GINT_TO_POINTER (i)))
        {
          gboolean same = t->tracks->len == match->tracks->len;
          for (guint j = 0; same && j < t->tracks->len; j++)
            same = bro_track_kind (t->tracks->pdata[j]) == bro_track_kind (match->tracks->pdata[j]);
          if (!same)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "The tracks of title %d differ from the listing the tracks were chosen on. Open the disc again and choose the tracks.", i);
              return NULL;
            }
        }
      g_hash_table_insert (map, GINT_TO_POINTER (i), GINT_TO_POINTER (match->index + 1));
      g_hash_table_add (used, GINT_TO_POINTER (match->index));
    }
  return g_steal_pointer (&map);
}

/* Checks a ripped file against its title in the disc listing. Returns why it can't be trusted, or NULL. */
static char *
verify_rip (Ctx *c, const char *file, BroTitle *title)
{
  g_autofree char *base = g_path_get_basename (file);
  g_autofree char *ph = NULL;
  g_autoptr (GPtrArray) problems = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) notes = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroMkvProbe) probe = NULL;
  const char *argv[] = { c->req->mkvmerge, "-J", file, NULL };
  GString *out;
  int status = -1;
  gboolean ran;

  if (!c->req->drive->archive.verify_rips)
    return NULL;
  if (!c->req->mkvmerge)
    {
      if (!c->reported_missing_verifier)
        log_line (c, BRO_SEV_WARNING, "Ripped files can't be checked against the disc listing without mkvmerge (MKVToolNix)");
      c->reported_missing_verifier = TRUE;
      return NULL;
    }
  ph = g_strdup_printf ("Checking %s", base);
  phase (c, ph);
  out = g_string_new (NULL);
  ran = bro_process_run (argv, NULL, NULL, 300, c->req->cancellable, collect_line, out, &status, NULL, NULL, NULL);
  if (ran && status >= 0 && status <= 1)
    probe = bro_mkv_probe_parse (out->str);
  g_string_free (out, TRUE);
  if (!probe)
    return cancelled (c) ? NULL : g_strdup_printf ("mkvmerge can't read %s", base);
  bro_rip_check (probe, title, problems, notes);
  for (guint i = 0; i < notes->len; i++)
    log_line (c, BRO_SEV_WARNING, "%s: %s", base, (char *) notes->pdata[i]);
  if (problems->len)
    {
      g_ptr_array_add (problems, NULL);
      g_autofree char *joined = g_strjoinv ("; ", (char **) problems->pdata);
      return g_strdup_printf ("%s doesn't match the disc listing: %s", base, joined);
    }
  {
    g_autofree char *len = probe->has_duration ? hms ((int) (probe->duration + 0.5)) : g_strdup ("?");
    log_line (c, BRO_SEV_INFO, "Checked %s: %s, %u track(s), %d chapter(s)", base, len, probe->track_types->len, probe->chapters);
  }
  return NULL;
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
  g_hash_table_remove_all (c->file_titles);
  prepare_episodes (c, src, info, indices);
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
      s = run_makemkv (c, args, TRUE, error);
      c->env = c->base_env;
      if (!s)
        return FALSE;

      after = mkv_files (out_dir);
      g_hash_table_iter_init (&it, after);
      while (g_hash_table_iter_next (&it, &key, NULL))
        if (!g_hash_table_contains (before, key))
          g_ptr_array_add (produced, g_strdup (key));
      g_ptr_array_sort (produced, (GCompareFunc) g_strcmp0);

      gboolean failed = s->exit_status != 0 || s->failed > 0 || produced->len == 0;
      if (failed)
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
          if (failed)
            {
              /* A file of a title that didn't finish keeps MakeMKV's name, so it can't pass for a finished one. */
              g_autofree char *base = g_path_get_basename (file);
              log_line (c, BRO_SEV_WARNING, "Kept %s under MakeMKV's name: the title did not finish", base);
              g_ptr_array_add (c->res->files, g_strdup (file));
              continue;
            }
          if (title)
            {
              g_autofree char *problem = verify_rip (c, file, title);
              if (problem)
                {
                  g_ptr_array_add (failures, g_strdup_printf ("title %d: %s", index, problem));
                  log_line (c, BRO_SEV_ERROR, "Title %d failed the check: %s", index, problem);
                  g_ptr_array_add (c->res->files, g_strdup (file));
                  continue;
                }
            }
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
              g_hash_table_insert (c->file_titles, g_strdup (final), GINT_TO_POINTER (index + 1));
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
  return split_episodes (c, out_dir, info, error);
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
  {
    g_autofree char *templ_name = backup_name (c, decrypt);
    if (c->req->drive->rip.backup_format == BRO_BACKUP_ISO)
      {
        g_autofree char *name = *templ_name ? g_strdup (templ_name)
                                             : bro_sanitize_component (c->res->disc_label && *c->res->disc_label ? c->res->disc_label : "disc");
        g_autofree char *iso_name = g_strconcat (name, ".iso", NULL);
        g_autofree char *iso = g_build_filename (dest, iso_name, NULL);
        g_free (dest);
        dest = bro_unique_path (iso);
      }
    else if (*templ_name)
      {
        g_autofree char *folder = g_build_filename (dest, templ_name, NULL);
        g_free (dest);
        dest = bro_unique_path (folder);
        g_mkdir_with_parents (dest, 0755);
      }
  }
  phase (c, decrypt ? "Backing up disc (decrypted)" : "Backing up disc");
  update (c, BRO_UPDATE_PROGRESS, BRO_SEV_INFO, NULL, 0, 0);
  c->expected_index = src->index;
  g_free (c->expected_device);
  c->expected_device = g_strdup (src->path);
  args = bro_makemkv_backup_args (c->env, src, decrypt, dest, &c->req->drive->rip);
  s = run_makemkv (c, args, TRUE, error);
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
  if (c->req->drive->archive.verify_rips)
    {
      g_autofree char *problem = bro_backup_problem (dest, c->req->drive->rip.backup_format == BRO_BACKUP_ISO);
      if (problem)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Backup failed the check: %s", problem);
          return NULL;
        }
    }
  log_line (c, BRO_SEV_INFO, "Backup saved to %s", dest);
  /* Without a disc listing a UHD disc looks like a plain Blu-ray; the backup's index.bdmv tells them apart. */
  if (c->req->drive->rip.backup_format == BRO_BACKUP_FOLDER)
    {
      BroDiscFormat found = bro_detect_backup_folder (dest);
      if (found != BRO_FORMAT_UNKNOWN && (!c->identity || found != c->identity->format))
        {
          g_autofree char *renamed = NULL;
          g_autofree char *current = g_path_get_basename (dest);
          resolve_identity (c, c->res->info, 0, found);
          renamed = backup_name (c, decrypt);
          if (*renamed && !g_str_equal (renamed, current))
            {
              g_autofree char *parent = g_path_get_dirname (dest);
              g_autofree char *want = g_build_filename (parent, renamed, NULL);
              char *target = bro_unique_path (want);
              if (g_rename (dest, target) == 0)
                {
                  g_autofree char *tn = g_path_get_basename (target);
                  log_line (c, BRO_SEV_INFO, "Renamed the backup to %s", tn);
                  g_free (dest);
                  dest = target;
                }
              else
                g_free (target);
            }
        }
    }
  return g_steal_pointer (&dest);
}

/* Moves title and track choices made on one listing to the title numbers of another (the listing read when the
 * job starts, or the listing of the backup). Fails when a chosen title can't be found. */
static gboolean
apply_title_map (Ctx *c, BroDiscInfo *old, BroDiscInfo *now, GError **error)
{
  g_autoptr (GArray) chosen = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GHashTable) map = NULL;
  g_autoptr (GString) moved = g_string_new (NULL);
  GHashTableIter it;
  gpointer k, v;
  for (guint i = 0; c->req->manual_titles && i < c->req->manual_titles->len; i++)
    g_array_append_val (chosen, g_array_index (c->req->manual_titles, int, i));
  g_hash_table_iter_init (&it, c->req->track_selections);
  while (g_hash_table_iter_next (&it, &k, NULL))
    {
      int i = GPOINTER_TO_INT (k);
      g_array_append_val (chosen, i);
    }
  g_hash_table_iter_init (&it, c->req->name_overrides);
  while (g_hash_table_iter_next (&it, &k, NULL))
    {
      int i = GPOINTER_TO_INT (k);
      g_array_append_val (chosen, i);
    }
  if (chosen->len == 0)
    return TRUE;
  map = bro_listing_map (chosen, old, now, c->req->track_selections, error);
  if (!map)
    return FALSE;
#define MAPPED(i) (GPOINTER_TO_INT (g_hash_table_lookup (map, GINT_TO_POINTER (i))) - 1)
  g_array_sort (chosen, cmp_int);
  for (guint i = 0; i < chosen->len; i++)
    {
      int from = g_array_index (chosen, int, i);
      if (i > 0 && g_array_index (chosen, int, i - 1) == from)
        continue;
      if (MAPPED (from) != from)
        g_string_append_printf (moved, "%s%d → %d", moved->len ? ", " : "", from, MAPPED (from));
    }
  if (moved->len)
    log_line (c, BRO_SEV_INFO, "Title numbers changed: %s", moved->str);
  if (c->req->manual_titles)
    {
      GArray *m = g_array_new (FALSE, FALSE, sizeof (int));
      for (guint i = 0; i < c->req->manual_titles->len; i++)
        {
          int to = MAPPED (g_array_index (c->req->manual_titles, int, i));
          if (to >= 0)
            g_array_append_val (m, to);
        }
      g_array_sort (m, cmp_int);
      g_array_unref (c->req->manual_titles);
      c->req->manual_titles = m;
    }
  {
    GHashTable *tracks = bro_track_selections_new ();
    g_hash_table_iter_init (&it, c->req->track_selections);
    while (g_hash_table_iter_next (&it, &k, &v))
      if (MAPPED (GPOINTER_TO_INT (k)) >= 0)
        g_hash_table_insert (tracks, GINT_TO_POINTER (MAPPED (GPOINTER_TO_INT (k))), g_hash_table_ref (v));
    g_hash_table_unref (c->req->track_selections);
    c->req->track_selections = tracks;
  }
  {
    GHashTable *names = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    g_hash_table_iter_init (&it, c->req->name_overrides);
    while (g_hash_table_iter_next (&it, &k, &v))
      if (MAPPED (GPOINTER_TO_INT (k)) >= 0)
        g_hash_table_insert (names, GINT_TO_POINTER (MAPPED (GPOINTER_TO_INT (k))), g_strdup (v));
    g_hash_table_unref (c->req->name_overrides);
    c->req->name_overrides = names;
  }
#undef MAPPED
  return TRUE;
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
  BroDiscInfo *opened = req->preloaded;
  BroDiscInfo *info = NULL;
  gboolean need_info, using_opened = FALSE;
  g_autofree char *final_dir = NULL, *out_dir = NULL, *short_id = NULL;

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

  /* The listing is always read again: titles chosen on an opened disc are only ripped when the disc in the drive
   * is still that disc and the titles can be found in the new listing. */
  if (opened)
    log_line (c, BRO_SEV_INFO, "Reading the disc listing again to check that the disc hasn't changed since it was opened");
  need_info = bro_rip_mode_makes_mkv (req->mode) || req->mode == BRO_MODE_INFO_ONLY;
  if (need_info)
    {
      info = scan_disc (c, req->source, error);
      if (!info)
        return FALSE;
    }
  else
    {
      /* Backups don't need the listing, but it tells DVD, Blu-ray and 4K UHD apart and gives the title. */
      g_autoptr (GError) err = NULL;
      info = scan_disc (c, req->source, &err);
      if (!info)
        {
          if (cancelled (c) || g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              g_propagate_error (error, g_steal_pointer (&err));
              return FALSE;
            }
          if (opened)
            {
              info = bro_disc_info_ref (opened);
              using_opened = TRUE;
              log_line (c, BRO_SEV_WARNING, "Using the listing read when the disc was opened: %s", err ? err->message : "unknown error");
            }
          else
            log_line (c, BRO_SEV_WARNING, "Continuing without the disc listing: %s", err ? err->message : "unknown error");
        }
    }
  if (opened && info && !using_opened)
    {
      g_autofree char *why = bro_listing_different_disc (opened, info);
      if (why)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "This is not the disc that was opened: %s. Open the disc again.", why);
          bro_disc_info_unref (info);
          return FALSE;
        }
      if (!apply_title_map (c, opened, info, error))
        {
          bro_disc_info_unref (info);
          return FALSE;
        }
    }
  c->res->info = info;
  if (info && *bro_disc_info_name (info))
    {
      g_free (c->res->disc_label);
      c->res->disc_label = g_strdup (bro_disc_info_name (info));
      update (c, BRO_UPDATE_DISC_LABEL, BRO_SEV_INFO, c->res->disc_label, 0, 0);
    }
  resolve_identity (c, info, 0, -1);

  final_dir = resolve_output_dir (c, error);
  if (!final_dir)
    return FALSE;
  c->output_dir = g_strdup (final_dir);
  c->res->output_dir = g_strdup (final_dir);
  update (c, BRO_UPDATE_OUTPUT_DIR, BRO_SEV_INFO, final_dir, 0, 0);
  /* Everything is written to a hidden staging folder first and only moved into the output folder once the job has
   * finished and passed its checks. */
  short_id = g_ascii_strdown (req->job_id, MIN (strlen (req->job_id), 8));
  {
    g_autofree char *name = g_strconcat (BRO_STAGING_PREFIX, short_id, NULL);
    out_dir = g_build_filename (final_dir, name, NULL);
  }
  c->work_dir = g_strdup (out_dir);
  if (g_mkdir_with_parents (out_dir, 0755) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create %s", out_dir);
      return FALSE;
    }
  log_line (c, BRO_SEV_INFO, "Output folder: %s (files are moved there once the job has finished and been checked)", final_dir);

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
        if (info && !apply_title_map (c, info, binfo, error))
          {
            bro_disc_info_unref (binfo);
            return FALSE;
          }
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
    default: return status == BRO_JOB_FAILED || status == BRO_JOB_CANCELLED || status == BRO_JOB_COMPLETED_WITH_ERRORS;
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
  g_autoptr (GPtrArray) steps_arr = g_ptr_array_new ();
  g_auto (GStrv) base_env = g_get_environ ();
  g_autofree char *code = c->identity ? bro_identity_format_code (c->identity) : g_strdup ("");
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
  if (c->identity)
    {
      g_autofree char *season = c->identity->label.season >= 0 ? g_strdup_printf ("%d", c->identity->label.season) : g_strdup ("");
      g_autofree char *disc = c->identity->label.disc >= 0 ? g_strdup_printf ("%d", c->identity->label.disc) : g_strdup ("");
      g_autofree char *set = bro_label_set_description (&c->identity->label);
      SETENV ("BROMELIA_NAME", c->identity->name);
      SETENV ("BROMELIA_KIND", bro_kind_token (c->identity->kind));
      SETENV ("BROMELIA_FORMAT", code);
      SETENV ("BROMELIA_ENCRYPTED", c->identity->encrypted ? "1" : "0");
      SETENV ("BROMELIA_SEASON", season);
      SETENV ("BROMELIA_DISC_NUMBER", disc);
      SETENV ("BROMELIA_DISC_SET", set);
    }
  if (c->checksum_file)
    SETENV ("BROMELIA_CHECKSUMS", c->checksum_file);
#undef SETENV
  /* The drive's steps, then the global plugins; each limited by its name / format conditions. */
  for (int pass = 0; pass < 2; pass++)
    {
      GPtrArray *src = pass == 0 ? c->req->drive->post_process : c->req->config->plugins;
      for (guint i = 0; i < src->len; i++)
        if (bro_plugin_matches (src->pdata[i], c->identity ? c->identity->name : c->res->disc_label, c->res->disc_label, code))
          g_ptr_array_add (steps_arr, src->pdata[i]);
    }
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
  if (c->identity)
    {
      g_autofree char *code = bro_identity_format_code (c->identity);
      S ("name", c->identity->name);
      S ("kind", bro_kind_token (c->identity->kind));
      S ("format", bro_format_token (c->identity->format));
      S ("formatCode", code);
      json_builder_set_member_name (b, "encrypted");
      json_builder_add_boolean_value (b, c->identity->encrypted);
      add_optional_int (b, "season", c->identity->label.season);
      add_optional_int (b, "discNumber", c->identity->label.disc);
    }
  build_episodes (b, c);
  if (c->checksum_file)
    S ("checksumFile", c->checksum_file);
  build_checksums (b, "checksums", c);
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
  c.separate = g_hash_table_new (g_direct_hash, g_direct_equal);
  c.file_titles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  c.episodes = g_ptr_array_new_with_free_func ((GDestroyNotify) episode_rec_free);
  c.error_messages = g_ptr_array_new_with_free_func (g_free);
  c.data_errors = g_ptr_array_new_with_free_func (g_free);
  c.first_episode = 1;
  c.episode_width = 2;
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
  if (status == BRO_JOB_SUCCEEDED && c.data_errors->len)
    {
      status = BRO_JOB_COMPLETED_WITH_ERRORS;
      res->error = g_strdup_printf ("MakeMKV reported %u read error(s) while reading the disc, so the files may be damaged. "
                                    "They were kept apart from finished archives.", c.data_errors->len);
      log_line (&c, BRO_SEV_ERROR, "%s", res->error);
    }
  status = finish_output (&c, status);

  write_manifest (&c, status);
  if (!g_cancellable_is_cancelled (req->cancellable) && run_post_processing (&c, status) && status == BRO_JOB_SUCCEEDED)
    {
      status = BRO_JOB_FAILED;
      g_free (res->error);
      res->error = g_strdup ("A post-processing step failed");
    }

  if (!req->skip_eject && req->source->kind == BRO_SOURCE_DRIVE &&
      ((status == BRO_JOB_SUCCEEDED && req->drive->automation.eject_when_done) ||
       ((status == BRO_JOB_FAILED || status == BRO_JOB_COMPLETED_WITH_ERRORS) && req->drive->automation.eject_on_failure)))
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
  if (req->drive->archive.archive_record && res->output_dir && res->files->len)
    {
      g_autofree char *want = g_build_filename (res->output_dir, "bromelia-log.txt", NULL);
      g_autofree char *target = bro_unique_path (want);
      g_autofree char *contents = NULL;
      gsize len = 0;
      if (g_file_get_contents (logpath, &contents, &len, NULL))
        g_file_set_contents (target, contents, len, NULL);
    }
  if (c.base_env) bro_makemkv_env_free (c.base_env);
  if (c.all_env) bro_makemkv_env_free (c.all_env);
  if (c.rip_titles) g_array_unref (c.rip_titles);
  g_free (c.last_total);
  g_free (c.expected_device);
  g_ptr_array_unref (c.commands);
  bro_identity_free (c.identity);
  if (c.videots) bro_videots_free (c.videots);
  if (c.plan) bro_episode_plan_free (c.plan);
  g_hash_table_unref (c.separate);
  g_hash_table_unref (c.file_titles);
  g_ptr_array_unref (c.episodes);
  if (c.checksums) g_ptr_array_unref (c.checksums);
  g_free (c.checksum_file);
  g_free (c.makemkv_version);
  g_ptr_array_unref (c.error_messages);
  g_ptr_array_unref (c.data_errors);
  g_free (c.output_dir);
  g_free (c.work_dir);
  return res;
}
