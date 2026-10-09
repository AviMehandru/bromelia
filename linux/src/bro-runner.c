/* bro-runner.c — job pipeline, post-processing and mkvmerge remuxing. */
#include "bro-runner.h"
#include "bro-archive.h"
#include "bro-dvd.h"
#include "bro-identity.h"
#include "bro-integrations.h"
#include "bro-logic.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <signal.h>
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
  r->online_id = g_strdup ("");
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
  g_free (r->online_id);
  g_free (r->makemkvcon);
  g_free (r->mkvmerge);
  g_clear_object (&r->cancellable);
  if (r->archived) g_ptr_array_unref (r->archived);
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
  g_free (r->libre_drive);
  g_free (r->name);
  bro_background_work_free (r->background);
  g_free (r->fingerprint);
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
  char *first_error;   /* first error other than the "N titles saved, M failed" summary: usually the cause */
  char *space_warning; /* MakeMKV's warning that the output may not fit (message 5038) */
} Summary;

/* Why a run failed, for the job's error message. */
static char *
summary_reason (Summary *s)
{
  if (s->first_error)
    return g_strdup (s->first_error);
  if (s->errors->len)
    return g_strdup (g_ptr_array_index (s->errors, s->errors->len - 1));
  return g_strdup_printf ("makemkvcon exit status %d", s->exit_status);
}

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
  GCancellable *run_cancel;    /* stops the running makemkvcon only (drive changed, not enough space) */
  BroNotice problem;           /* a problem with MakeMKV or the drive that explains a failure */
  BroDiscInfo *rip_info;       /* the listing the titles were ripped from (disc, backup or one-pass listing) */
  BroMediaMatch *metadata;     /* the movie / show found online */
  gboolean metadata_chosen;    /* metadata is the id chosen for this disc (not a search result) */
  int looked_up_kind;          /* the kind the online lookup searched for, -1 before it ran */
  GHashTable *episode_details; /* episode number -> BroEpisodeDetails*, from the online lookup */
  int main_title;              /* the main feature of a movie (the longest title ripped), for media server names; -1 */
  char *fingerprint;           /* of the disc listing (bro_disc_fingerprint), or NULL */
  char *already_archived;      /* why an automatic rip stopped: the disc was archived before (see check_already_archived) */
  char *files_folder;          /* the folder finish_output moved the files into (also one marked [INCOMPLETE] / [READ ERRORS]) */
  FILE *makemkv_log;           /* everything makemkvcon printed (makemkv.txt in the job folder) */
  char *debug_log;             /* MakeMKV's debug log (app_ShowDebug) of the running makemkvcon, named by message 1004 */
} Ctx;

typedef struct {
  char *file; /* relative to the output folder */
  int episode, source, first, last;
  char *title; /* from the online lookup, or NULL */
} EpisodeRec;

static void
episode_rec_free (EpisodeRec *e)
{
  g_free (e->file);
  g_free (e->title);
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
  g_free (s->first_error);
  g_free (s->space_warning);
  g_free (s);
}

static void
on_makemkv_line (const char *line, gpointer data)
{
  Ctx *c = data;
  Summary *s = c->sum;
  g_autoptr (BroEvent) ev = NULL;
  if (c->makemkv_log)
    {
      fprintf (c->makemkv_log, "%s\n", line);
      fflush (c->makemkv_log);
    }
  ev = bro_event_parse (line);
  if (!ev)
    return;
  bro_disc_info_consume (s->info, ev);
  switch (ev->type)
    {
    case BRO_EV_MESSAGE:
      {
        BroSeverity sev = bro_event_severity (ev);
        g_autofree char *detail = NULL;
        BroNotice notice = bro_notice_from_event (ev, &detail);
        if (notice == BRO_NOTICE_LIBREDRIVE && !c->res->libre_drive)
          c->res->libre_drive = g_steal_pointer (&detail);
        else if (notice != BRO_NOTICE_NONE && notice != BRO_NOTICE_LIBREDRIVE && c->problem == BRO_NOTICE_NONE)
          c->problem = notice;
        /* "Debug logging enabled, log will be saved as file:///…/MakeMKV_log.txt" */
        if (ev->code == 1004 && ev->params->len > 0 && g_str_has_prefix (ev->params->pdata[0], "file://"))
          {
            const char *p = (const char *) ev->params->pdata[0] + strlen ("file://");
            g_free (c->debug_log);
            c->debug_log = g_uri_unescape_string (p, NULL);
            if (!c->debug_log)
              c->debug_log = g_strdup (p);
          }
        if (sev == BRO_SEV_DEBUG && !c->show_debug)
          return;
        log_line (c, sev, "%s", ev->text);
        if (sev == BRO_SEV_ERROR)
          {
            if (!s->first_error && ev->code != 5037 && ev->code != 5004)
              s->first_error = g_strdup (ev->text);
            g_ptr_array_add (s->errors, g_strdup (ev->text));
            g_ptr_array_add (c->error_messages, g_strdup (ev->text));
            if (c->collect_data_errors)
              g_ptr_array_add (c->data_errors, g_strdup (ev->text));
          }
        if (ev->code == 1005 && !c->makemkv_version)
          c->makemkv_version = g_strdup (ev->params->len ? ev->params->pdata[0] : ev->text);
        if ((ev->code == 5036 || ev->code == 5005) && ev->params->len > 0)
          s->saved = atoi (ev->params->pdata[0]);
        if (ev->code == 5038 && !s->space_warning)
          {
            /* "The total size of all output files may reach as much as … while there are only … free": stop before
             * anything is written rather than fail when the disk fills up. */
            s->space_warning = g_strdup (ev->text);
            if (c->run_cancel)
              g_cancellable_cancel (c->run_cancel);
          }
        if ((ev->code == 5037 || ev->code == 5004) && ev->params->len > 1)
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
          if (c->run_cancel)
            g_cancellable_cancel (c->run_cancel);
        }
      break;
    case BRO_EV_RAW:
      log_line (c, BRO_SEV_INFO, "%s", ev->text);
      break;
    default:
      break;
    }
}

/* Appends text to a file of the job folder. */
static void
append_job_file (Ctx *c, const char *name, const char *text, gsize len)
{
  g_autofree char *path = g_build_filename (c->req->job_dir, name, NULL);
  FILE *f = fopen (path, "a");
  if (!f)
    return;
  fwrite (text, 1, len, f);
  fclose (f);
}

/* MakeMKV starts its debug log afresh every time it starts, so it is added to the job's copy after each run. */
static void
keep_debug_log (Ctx *c, const char *cmd)
{
  g_autofree char *contents = NULL;
  g_autofree char *head = NULL;
  gsize len = 0;
  if (!c->debug_log || !g_file_get_contents (c->debug_log, &contents, &len, NULL) || len == 0)
    return;
  head = g_strdup_printf ("==== %s after $ %s\n", c->debug_log, cmd);
  append_job_file (c, BRO_MAKEMKV_DEBUG_LOG, head, strlen (head));
  append_job_file (c, BRO_MAKEMKV_DEBUG_LOG, contents, len);
  if (contents[len - 1] != '\n')
    append_job_file (c, BRO_MAKEMKV_DEBUG_LOG, "\n", 1);
}

static char *
now_stamp (void)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  return g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
}

static void
cancel_run (GCancellable *job, gpointer run)
{
  g_cancellable_cancel (run);
}

/* Runs makemkvcon with the current environment. Returns NULL (with error) when it could not run or was cancelled.
 * With reads_data, error messages count as read errors (rips and backups, not listings). */
static Summary *
run_makemkv (Ctx *c, GPtrArray *args, gboolean reads_data, GError **error)
{
  Summary *s = g_new0 (Summary, 1);
  g_auto (GStrv) envp = bro_makemkv_env_environ (c->env);
  g_autofree char *cmd = NULL;
  g_autoptr (GCancellable) run_cancel = g_cancellable_new ();
  /* makemkvcon ignores SIGINT, so it is stopped with SIGTERM. */
  BroRunOptions opt = { 0, MAX (0, c->req->config->stall_timeout_minutes) * 60, SIGTERM };
  BroRunStatus st = { 0 };
  gulong handler;
  gboolean ran;

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
  g_clear_pointer (&c->debug_log, g_free);
  if (!c->makemkv_log)
    {
      g_autofree char *path = g_build_filename (c->req->job_dir, BRO_MAKEMKV_LOG, NULL);
      c->makemkv_log = fopen (path, "a");
    }
  if (c->makemkv_log)
    {
      g_autofree char *stamp = now_stamp ();
      fprintf (c->makemkv_log, "==== %s $ %s\n", stamp, cmd);
      fflush (c->makemkv_log);
    }

  c->sum = s;
  c->collect_data_errors = reads_data;
  c->run_cancel = run_cancel;
  /* The job's cancellable stops this run too. */
  handler = c->req->cancellable ? g_cancellable_connect (c->req->cancellable, G_CALLBACK (cancel_run), run_cancel, NULL) : 0;
  ran = bro_process_run_ex ((const char *const *) args->pdata, (const char *const *) envp, c->env->home, &opt, run_cancel,
                            on_makemkv_line, c, &st, error);
  if (handler)
    g_cancellable_disconnect (c->req->cancellable, handler);
  c->run_cancel = NULL;
  c->collect_data_errors = FALSE;
  c->sum = NULL;
  s->exit_status = st.exit_status;
  if (c->makemkv_log)
    {
      g_autofree char *stamp = now_stamp ();
      if (ran)
        fprintf (c->makemkv_log, "==== %s exit status %d\n", stamp, st.exit_status);
      else
        fprintf (c->makemkv_log, "==== could not start: %s\n", error && *error ? (*error)->message : "unknown error");
      fflush (c->makemkv_log);
    }
  keep_debug_log (c, cmd);
  if (!ran)
    {
      summary_free (s);
      if (error && *error && !g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_prefix_error (error, "Could not start makemkvcon: ");
      return NULL;
    }
  /* These stop makemkvcon themselves, so they are checked before cancellation. */
  if (!cancelled (c) && (s->drive_mismatch || s->space_warning || st.stalled))
    {
      if (s->drive_mismatch)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, s->drive_mismatch);
      else if (s->space_warning)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Not enough free space on the destination, so the rip was stopped before writing. MakeMKV: “%s”", s->space_warning);
      else
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "makemkvcon printed nothing for %d minutes and was stopped%s. The drive or disc may be stuck: eject the disc and retry.",
                     c->req->config->stall_timeout_minutes, st.abandoned ? " (it did not exit; the drive may need to be reset)" : "");
      summary_free (s);
      return NULL;
    }
  if (st.cancelled || st.abandoned || cancelled (c))
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
  {
    BroMediaMatch *m = c->metadata;
    g_autofree char *year = m && m->year ? g_strdup_printf ("%d", m->year) : g_strdup ("");
    g_autofree char *tmdb = m && m->tmdb_id ? g_strdup_printf ("%d", m->tmdb_id) : g_strdup ("");
    const char *season = g_hash_table_lookup (v, "season");
    g_autofree char *season_or_1 = g_strdup (season && *season ? season : "1");
    bro_template_values_set (v, "releaseYear", year);
    bro_template_values_set (v, "tmdb", tmdb);
    bro_template_values_set (v, "imdb", m && m->imdb_id ? m->imdb_id : "");
    bro_template_values_set (v, "libraryFolder", g_strcmp0 (g_hash_table_lookup (v, "kind"), "tv") == 0 ? "TV Shows" : "Movies");
    bro_template_values_set (v, "seasonOr1", season_or_1);
  }
  return v;
}

static gboolean
media_server (Ctx *c)
{
  return c->req->drive->output.layout == BRO_LAYOUT_MEDIA_SERVER;
}

/* The output root with ~ expanded. */
static char *
output_root (Ctx *c)
{
  const char *root_t = bro_app_config_output_root (c->req->config, c->req->drive);
  if (!root_t || !*root_t)
    root_t = "~/Videos/Bromelia";
  return g_str_has_prefix (root_t, "~") ? g_build_filename (g_get_home_dir (), root_t + 1, NULL) : g_strdup (root_t);
}

static void
resolve_identity (Ctx *c, BroDiscInfo *info, int play_all, int format)
{
  int fmt = format >= 0 ? format : (c->identity ? (int) c->identity->format : -1);
  BroIdentity *id = bro_identity_resolve (info, c->res->disc_label, c->req->disc_flags, fmt, c->req->mode == BRO_MODE_BACKUP,
                                          c->req->media_name, c->req->media_kind, play_all);
  if (c->metadata)
    {
      g_free (id->name);
      id->name = bro_sanitize_component (c->metadata->title);
      /* An id chosen for the disc may be a show where a movie was assumed (or the other way round). */
      if (c->metadata_chosen && c->metadata->kind >= 0 && c->metadata->kind != (int) id->kind && c->req->media_kind < 0)
        {
          id->kind = c->metadata->kind;
          g_free (id->reason);
          id->reason = g_strdup_printf ("%s lists it as a %s", c->metadata->provider, id->kind == BRO_KIND_TV ? "TV show" : "movie");
        }
    }
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
  g_autofree char *root = output_root (c);
  g_autoptr (GHashTable) v = base_values (c, BRO_JOB_RUNNING);
  g_autofree char *rel = bro_template_render_path (media_server (c) ? BRO_MEDIA_FOLDER_TEMPLATE : c->req->drive->output.folder_template, v);
  char *dir;
  gboolean non_empty = FALSE;
  /* Only a folder made for this disc may be renamed or removed; never the output root itself. */
  gboolean owned = *rel != '\0';

  dir = *rel ? g_build_filename (root, rel, NULL) : g_strdup (root);
  /* A media server library keeps one folder per movie / show: later discs are added to it. */
  if (media_server (c))
    {
      c->owns_output_dir = FALSE;
      if (g_mkdir_with_parents (dir, 0755) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create %s", dir);
          g_free (dir);
          return NULL;
        }
      return dir;
    }

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
  {
    BroEpisodeDetails *d = g_hash_table_lookup (c->episode_details, GINT_TO_POINTER (episode));
    e->title = d ? g_strdup (d->title) : NULL;
  }
  g_ptr_array_add (c->episodes, e);
}

/* Template values of episode n. */
static void
set_episode (Ctx *c, GHashTable *v, int n)
{
  g_autofree char *label = bro_episode_label (n, c->episode_width);
  g_autofree char *num = g_strdup_printf ("%d", n);
  BroEpisodeDetails *d = g_hash_table_lookup (c->episode_details, GINT_TO_POINTER (n));
  bro_template_values_set (v, "episode", label);
  bro_template_values_set (v, "episodeNumber", num);
  bro_template_values_set (v, "episodeTitle", d ? d->title : "");
}

static char *
rename_file (Ctx *c, const char *file, BroTitle *title, int ordinal, BroDiscInfo *info, const char *out_dir)
{
  const char *explicit_name = g_hash_table_lookup (c->req->name_overrides, GINT_TO_POINTER (title->index));
  const char *tmpl;
  g_autoptr (GHashTable) v = base_values (c, BRO_JOB_RUNNING);
  g_autofree char *track = bro_track_label (title);
  int k = GPOINTER_TO_INT (g_hash_table_lookup (c->separate, GINT_TO_POINTER (title->index))) - 1;
  char *final;

  title_values (v, title, ordinal, info, file);
  bro_template_values_set (v, "track", track);
  if (k >= 0)
    set_episode (c, v, c->first_episode + k);
  tmpl = explicit_name && *explicit_name ? explicit_name
         : media_server (c) ? bro_media_server_file_template (v, title->index == c->main_title)
         : c->req->drive->output.file_name_template;
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
static void look_up_metadata (Ctx *c, BroDiscInfo *info);

/* The last episode of the previous disc of this set (see bro_episode_continuation), for discs 2 and later. */
static BroContinuation *
previous_disc (Ctx *c)
{
  BroIdentity *id = c->identity;
  g_autofree char *root = NULL, *season_folder = NULL;
  g_autoptr (GPtrArray) folders = g_ptr_array_new ();
  int season;
  if (!id || id->label.disc <= 1)
    return NULL;
  phase (c, "Looking for the previous disc's episodes");
  season = id->label.season >= 0 ? id->label.season : 1;
  root = output_root (c);
  for (guint i = 0; c->req->archived && i < c->req->archived->len; i++)
    g_ptr_array_add (folders, ((BroArchivedCandidate *) c->req->archived->pdata[i])->folder);
  if (media_server (c) && c->output_dir)
    {
      g_autofree char *name = g_strdup_printf ("Season %02d", season);
      season_folder = g_build_filename (c->output_dir, name, NULL);
    }
  {
    BroContinuationQuery q = { id->name, id->label.title ? id->label.title : "", id->label.season, id->label.part, id->label.volume,
                               id->label.disc };
    return bro_episode_continuation (&q, root, folders, season_folder, season);
  }
}

/* The titles of this disc's episodes, when the show was found online (metadata.episodeTitles). A season whose numbers
 * don't cover every episode of the disc (numbered across seasons, say) gives no titles at all. */
static void
look_up_episode_titles (Ctx *c, int count)
{
  const BroMetadataConfig *m = &c->req->config->metadata;
  g_autoptr (GError) err = NULL;
  g_autoptr (GHashTable) all = NULL;
  g_autoptr (GString) missing = g_string_new (NULL), list = g_string_new (NULL);
  int season, lo = G_MAXINT, hi = 0;
  GHashTableIter it;
  gpointer key;
  g_hash_table_remove_all (c->episode_details);
  if (!m->episode_titles || count <= 0 || !c->metadata || c->identity->kind != BRO_KIND_TV)
    return;
  season = c->identity->label.season >= 0 ? c->identity->label.season : 1;
  phase (c, "Looking up episode titles");
  if (!(all = bro_metadata_episodes (c->metadata, season, m, &err)))
    {
      log_line (c, BRO_SEV_WARNING, "Looking up the episode titles failed: %s", err ? err->message : "unknown error");
      return;
    }
  if (!g_hash_table_size (all))
    {
      log_line (c, BRO_SEV_WARNING, "%s lists no episodes for season %d of “%s”", c->metadata->provider, season, c->metadata->title);
      return;
    }
  g_hash_table_iter_init (&it, all);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      lo = MIN (lo, GPOINTER_TO_INT (key));
      hi = MAX (hi, GPOINTER_TO_INT (key));
    }
  for (int n = c->first_episode; n < c->first_episode + count; n++)
    {
      BroEpisodeDetails *d = g_hash_table_lookup (all, GINT_TO_POINTER (n));
      if (!d)
        g_string_append_printf (missing, "%s%d", missing->len ? ", " : "", n);
      else
        g_string_append_printf (list, "%s%d “%s”", list->len ? ", " : "", n, d->title);
    }
  if (missing->len)
    {
      log_line (c, BRO_SEV_WARNING, "Season %d of “%s” on %s has episodes %d–%d, not %s; the episodes get no titles", season,
                c->metadata->title, c->metadata->provider, lo, hi, missing->str);
      return;
    }
  for (int n = c->first_episode; n < c->first_episode + count; n++)
    {
      gpointer d = NULL;
      if (g_hash_table_steal_extended (all, GINT_TO_POINTER (n), NULL, &d))
        g_hash_table_replace (c->episode_details, GINT_TO_POINTER (n), d);
    }
  log_line (c, BRO_SEV_INFO, "Episode titles from %s, season %d: %s", c->metadata->provider, season, list->str);
}

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
  /* The menus showed a TV show where the lookup searched for a movie: search again, with the name read from the disc. */
  if (c->looked_up_kind >= 0 && (int) c->identity->kind != c->looked_up_kind && !c->metadata_chosen)
    {
      g_clear_pointer (&c->metadata, bro_media_match_free);
      resolve_identity (c, info, strict ? (int) strict->starts->len : 0, -1);
      look_up_metadata (c, info);
    }
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
  {
    g_autoptr (BroContinuation) found = previous_disc (c);
    if (found && first < 0)
      {
        first = found->last_episode + 1;
        g_free (how);
        how = g_strdup_printf ("after episode %d of %s", found->last_episode, found->source);
      }
    else if (found && first != found->last_episode + 1)
      log_line (c, BRO_SEV_WARNING, "The previous disc ended with episode %d (%s), but this disc starts with %d (%s)", found->last_episode,
                found->source, first, how);
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
  look_up_episode_titles (c, count);
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
          bro_episode_plan_range (p, i, &f, &l);
          set_episode (c, vals, c->first_episode + (int) i);
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
      char *final = rename_to (c, parts->pdata[i], vals,
                               media_server (c) ? bro_media_server_file_template (vals, FALSE) : c->req->drive->output.file_name_template,
                               fallback, out_dir);
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
  /* Kept outside a media server library, where the server would pick the files up. */
  if (media_server (c))
    {
      g_autofree char *root = output_root (c);
      g_autofree char *base = g_path_get_basename (c->output_dir);
      g_autofree char *raw = g_strdup_printf ("%s [%s %s]", base, tag, short_id);
      g_autofree char *name = bro_sanitize_component (raw);
      g_autofree char *want = g_build_filename (root, name, NULL);
      *dest = bro_unique_path (want);
      g_mkdir_with_parents (*dest, 0755);
      *from = g_strdup (c->work_dir);
      return;
    }
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
  if (c->req->drive->archive.archive_record)
    {
      g_autofree char *raw = g_build_filename (c->req->job_dir, BRO_MAKEMKV_LOG, NULL);
      g_string_append_printf (t, "The job's log is in this folder as bromelia-log.txt%s.\n",
                              g_file_test (raw, G_FILE_TEST_EXISTS) ? ", and everything MakeMKV printed as makemkv-log.txt" : "");
    }
  g_string_append_printf (t, "Full log: %s\n", log);
  g_file_set_contents (path, t->str, -1, NULL);
}

/* A path under the staging folder, moved to where it (or the folder holding it) went (moved: relative path -> new
 * path; folders merged into existing ones have an entry for each item moved). */
static char *
relocate (const char *path, const char *stage, GHashTable *moved)
{
  g_autofree char *prefix = g_strconcat (stage, G_DIR_SEPARATOR_S, NULL);
  g_autofree char *key = NULL;
  const char *rest;
  if (!g_str_has_prefix (path, prefix))
    return g_strdup (path);
  rest = path + strlen (prefix);
  key = g_strdup (rest);
  for (;;)
    {
      const char *top = g_hash_table_lookup (moved, key);
      char *slash;
      if (top)
        return rest[strlen (key)] ? g_build_filename (top, rest + strlen (key) + 1, NULL) : g_strdup (top);
      slash = strrchr (key, G_DIR_SEPARATOR);
      if (!slash)
        return g_strdup (path);
      *slash = '\0';
    }
}

/* Moves from/rel to dest/rel (a unique name when taken). With merge, a folder that exists in dest already (a
 * media server's show or Season folder) gets the items instead. */
static void
move_item (Ctx *c, const char *from, const char *rel, const char *dest, gboolean merge, GHashTable *moved, gboolean *left)
{
  g_autofree char *source = g_build_filename (from, rel, NULL);
  g_autofree char *want = g_build_filename (dest, rel, NULL);
  char *target;
  if (merge && g_file_test (source, G_FILE_TEST_IS_DIR) && !g_file_test (source, G_FILE_TEST_IS_SYMLINK) &&
      g_file_test (want, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GPtrArray) items = visible_items (source);
      for (guint i = 0; i < items->len; i++)
        {
          g_autofree char *child = g_build_filename (rel, items->pdata[i], NULL);
          move_item (c, from, child, dest, merge, moved, left);
        }
      return;
    }
  target = bro_unique_path (want);
  if (g_rename (source, target) == 0)
    g_hash_table_insert (moved, g_strdup (rel), target);
  else
    {
      *left = TRUE;
      log_line (c, BRO_SEV_ERROR, "Could not move %s out of %s", rel, from);
      g_free (target);
    }
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
    move_item (c, from, items->pdata[i], dest, !tag && c->req->drive->output.layout == BRO_LAYOUT_MEDIA_SERVER, moved, &left);
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
  if (g_hash_table_size (moved))
    {
      g_free (c->files_folder);
      c->files_folder = g_strdup (dest);
    }
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

/* Writes text to path unless it exists; adds its name to written. */
static void
write_metadata_file (Ctx *c, const char *text, const char *path, GPtrArray *written)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GFileOutputStream) out = NULL;
  g_autoptr (GError) err = NULL;
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    return;
  if (!(out = g_file_create (file, G_FILE_CREATE_NONE, NULL, &err))
      || !g_output_stream_write_all (G_OUTPUT_STREAM (out), text, strlen (text), NULL, NULL, &err)
      || !g_output_stream_close (G_OUTPUT_STREAM (out), NULL, &err))
    {
      log_line (c, BRO_SEV_WARNING, "Could not write %s: %s", path, err ? err->message : "unknown error");
      return;
    }
  g_ptr_array_add (written, relative_to (path, c->res->output_dir));
}

/* path with its extension replaced by .nfo. */
static char *
nfo_path (const char *path)
{
  const char *slash = strrchr (path, '/'), *dot = strrchr (path, '.');
  return dot && (!slash || dot > slash) ? g_strdup_printf ("%.*s.nfo", (int) (dot - path), path) : g_strconcat (path, ".nfo", NULL);
}

/* Media server layout with an online match (metadata.nfo): the movie's or show's .nfo and poster in its folder (when
 * missing: an earlier disc may have written them, or the server rewritten them), and an .nfo next to each episode. These
 * are not in SHA256SUMS. Failures only log. */
static void
write_media_server_metadata (Ctx *c)
{
  const char *dir = c->res->output_dir;
  g_autoptr (GPtrArray) written = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *poster = NULL;
  if (!c->req->config->metadata.nfo || !media_server (c) || !c->metadata || !dir || !c->identity)
    return;
  phase (c, "Writing media server metadata");
  if (c->identity->kind == BRO_KIND_TV)
    {
      g_autofree char *text = bro_nfo (c->metadata, BRO_KIND_TV);
      g_autofree char *path = g_build_filename (dir, "tvshow.nfo", NULL);
      int season = c->identity->label.season >= 0 ? c->identity->label.season : 1;
      write_metadata_file (c, text, path, written);
      for (guint i = 0; i < c->episodes->len; i++)
        {
          EpisodeRec *e = c->episodes->pdata[i];
          BroEpisodeDetails *d = g_hash_table_lookup (c->episode_details, GINT_TO_POINTER (e->episode));
          g_autofree char *file = NULL, *nfo = NULL, *ep = NULL;
          if (!d)
            continue;
          file = g_build_filename (dir, e->file, NULL);
          nfo = nfo_path (file);
          ep = bro_episode_nfo (c->metadata->title, season, e->episode, d);
          write_metadata_file (c, ep, nfo, written);
        }
    }
  else if (c->main_title >= 0)
    {
      GHashTableIter it;
      gpointer k, v;
      g_hash_table_iter_init (&it, c->file_titles);
      while (g_hash_table_iter_next (&it, &k, &v))
        if (GPOINTER_TO_INT (v) - 1 == c->main_title)
          {
            g_autofree char *text = bro_nfo (c->metadata, BRO_KIND_MOVIE);
            g_autofree char *nfo = nfo_path (k);
            write_metadata_file (c, text, nfo, written);
            break;
          }
    }
  poster = g_build_filename (dir, BRO_POSTER_NAME, NULL);
  if (c->metadata->poster && *c->metadata->poster && !g_file_test (poster, G_FILE_TEST_EXISTS))
    {
      g_autoptr (GError) err = NULL;
      if (bro_http_download (c->metadata->poster, poster, &err))
        g_ptr_array_add (written, g_strdup (BRO_POSTER_NAME));
      else
        {
          g_autoptr (GUri) u = g_uri_parse (c->metadata->poster, G_URI_FLAGS_NONE, NULL);
          log_line (c, BRO_SEV_WARNING, "Could not download the poster from %s: %s", u && g_uri_get_host (u) ? g_uri_get_host (u) : "",
                    err ? err->message : "unknown error");
        }
    }
  if (written->len)
    {
      GString *list = g_string_new (NULL);
      for (guint i = 0; i < written->len && (written->len <= 4 || i < 3); i++)
        g_string_append_printf (list, "%s%s", i ? ", " : "", (char *) written->pdata[i]);
      if (written->len > 4)
        g_string_append_printf (list, " and %u more", written->len - 3);
      log_line (c, BRO_SEV_INFO, "Wrote media server metadata: %s", list->str);
      g_string_free (list, TRUE);
    }
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
      if (e->title)
        {
          json_builder_set_member_name (b, "title");
          json_builder_add_string_value (b, e->title);
        }
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
  BroDiscInfo *info = c->rip_info ? c->rip_info : c->res->info;
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
  if (c->fingerprint)
    S ("fingerprint", c->fingerprint);
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
  if (c->res->libre_drive)
    S ("libreDrive", c->res->libre_drive);
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
  g_autofree char *tmpl = g_strstrip (g_strdup (media_server (c) ? BRO_MEDIA_BACKUP_NAME : c->req->drive->output.file_name_template));
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

int
bro_one_pass_min_length (GArray *indices, BroDiscInfo *info, int current)
{
  g_autoptr (GHashTable) chosen = g_hash_table_new (g_direct_hash, g_direct_equal);
  int shortest = G_MAXINT, longest_other = -1, picked = 0;
  for (guint i = 0; i < indices->len; i++)
    g_hash_table_add (chosen, GINT_TO_POINTER (g_array_index (indices, int, i)));
  /* Rips with fewer titles gain nothing: the extra listing costs as much as the runs it saves. */
  if (g_hash_table_size (chosen) < 3 || g_hash_table_size (chosen) >= info->titles->len)
    return -1;
  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      int d = bro_title_duration (t);
      if (g_hash_table_contains (chosen, GINT_TO_POINTER (t->index)))
        {
          shortest = MIN (shortest, d);
          picked++;
        }
      else
        longest_other = MAX (longest_other, d);
    }
  /* Listed lengths are rounded to seconds: keep 2 s between the shortest chosen and the longest left out. */
  if (picked != (int) g_hash_table_size (chosen) || longest_other < 0 || shortest - longest_other < 2)
    return -1;
  return longest_other + 1 > MAX (current, 0) ? longest_other + 1 : -1;
}

gboolean
bro_one_pass_matches (BroDiscInfo *listing, GArray *indices, BroDiscInfo *info)
{
  g_autoptr (GHashTable) map = NULL;
  g_autoptr (GHashTable) targets = g_hash_table_new (g_direct_hash, g_direct_equal);
  GHashTableIter it;
  gpointer v;
  g_autoptr (GArray) unique = g_array_copy (indices);
  g_array_sort (unique, cmp_int);
  for (guint i = 1; i < unique->len;)
    if (g_array_index (unique, int, i) == g_array_index (unique, int, i - 1))
      g_array_remove_index (unique, i);
    else
      i++;
  if (listing->titles->len != unique->len)
    return FALSE;
  map = bro_listing_map (unique, info, listing, NULL, NULL);
  if (!map)
    return FALSE;
  g_hash_table_iter_init (&it, map);
  while (g_hash_table_iter_next (&it, NULL, &v))
    g_hash_table_add (targets, v);
  return g_hash_table_size (targets) == listing->titles->len;
}

gint64
bro_disk_available (const char *path)
{
  g_autofree char *dir = g_strdup (path);
  g_autoptr (GFile) f = NULL;
  g_autoptr (GFileInfo) fi = NULL;
  while (!g_file_test (dir, G_FILE_TEST_EXISTS) && strchr (dir, G_DIR_SEPARATOR) && strlen (dir) > 1)
    {
      char *parent = g_path_get_dirname (dir);
      g_free (dir);
      dir = parent;
    }
  f = g_file_new_for_path (dir);
  fi = g_file_query_filesystem_info (f, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, NULL);
  if (!fi || !g_file_info_has_attribute (fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    return -1;
  return (gint64) g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
}

gint64
bro_disk_required (gint64 bytes)
{
  /* A margin of 2 % or 256 MB, whichever is larger: listing sizes are estimates. */
  return bytes + MAX ((gint64) 256 << 20, bytes / 50);
}

/* Fails the job before ripping when the destination can't hold the chosen titles (sizes from the listing), plus a copy
 * of titles with hand-picked tracks (remuxed) and of a "play all" title (split into episodes). */
static gboolean
check_free_space (Ctx *c, BroDiscInfo *info, GArray *indices, const char *out_dir, GError **error)
{
  gint64 need = 0, free_bytes, required;
  g_autofree char *parent = g_path_get_dirname (out_dir);
  for (guint i = 0; i < indices->len; i++)
    {
      int index = g_array_index (indices, int, i);
      BroTitle *t = bro_disc_info_title (info, index);
      if (!t)
        continue;
      need += bro_title_size (t);
      if (g_hash_table_contains (c->req->track_selections, GINT_TO_POINTER (index)))
        need += bro_title_size (t);
      if (c->plan && bro_title_source_id (t) == c->plan->title)
        need += bro_title_size (t);
    }
  free_bytes = bro_disk_available (out_dir);
  if (need <= 0 || free_bytes < 0)
    return TRUE;
  required = bro_disk_required (need);
  {
    g_autofree char *f = g_format_size (free_bytes), *n = g_format_size (need), *r = g_format_size (required);
    log_line (c, BRO_SEV_INFO, "Free space: %s; the titles need about %s", f, n);
    if (free_bytes < required)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                     "Not enough free space in %s: the titles need about %s (with a margin), only %s is free.", parent, r, f);
        return FALSE;
      }
  }
  return TRUE;
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
  if (title->tracks->len > 0 && probe->track_types->len < title->tracks->len)
    log_line (c, BRO_SEV_INFO, "%s has %u of the %u tracks on the disc; the selection rule left out the rest (use “+sel:all” to keep every track)",
              base, probe->track_types->len, title->tracks->len);
  return NULL;
}

static gboolean apply_title_map (Ctx *c, BroDiscInfo *old, BroDiscInfo *now, GError **error);

/* Checks a listing read with a minimum length that leaves out every title but the chosen ones, so they can be ripped
 * in one run. Returns that listing (and the length), or NULL to rip title by title. */
static BroDiscInfo *
one_pass_listing (Ctx *c, const BroSource *src, BroDiscInfo *info, GArray *indices, int *min_length, GError **error)
{
  BroRipConfig rip = c->req->drive->rip;
  g_autoptr (GPtrArray) args = NULL;
  Summary *s;
  BroDiscInfo *listing;
  int length;
  if (g_hash_table_size (c->req->track_selections) > 0)
    return NULL;
  length = bro_one_pass_min_length (indices, info, rip.min_length_seconds);
  if (length < 0)
    return NULL;
  rip.min_length_seconds = length;
  phase (c, "Reading the disc listing for a one-pass rip");
  args = bro_makemkv_info_args (c->env, src, &rip);
  s = run_makemkv (c, args, FALSE, error);
  if (!s)
    return NULL;
  listing = bro_disc_info_ref (s->info);
  summary_free (s);
  if (!bro_one_pass_matches (listing, indices, info))
    {
      log_line (c, BRO_SEV_WARNING, "A minimum length of %d s doesn't leave exactly the chosen titles; ripping them one by one", length);
      bro_disc_info_unref (listing);
      return NULL;
    }
  if (!apply_title_map (c, info, listing, error))
    {
      bro_disc_info_unref (listing);
      return NULL;
    }
  log_line (c, BRO_SEV_INFO, "Ripping %u titles in one pass (a minimum title length of %d s leaves out the other %u)",
            indices->len, length, info->titles->len - indices->len);
  *min_length = length;
  return listing;
}

static gboolean
rip_titles (Ctx *c, const BroSource *src, BroDiscInfo *info, const char *out_dir, int extra_steps, GError **error)
{
  g_autoptr (GArray) indices = choose_titles (c, info, error);
  g_autoptr (GPtrArray) failures = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (BroDiscInfo) pass = NULL;
  BroRipConfig rip = c->req->drive->rip;
  gboolean every = TRUE, single;
  int count;

  if (!indices)
    return FALSE;
  /* Rip several titles in one makemkvcon run (one read of the disc structure) when they are exactly the titles above
   * some length: then a minimum length leaves just them, and "all" rips them. */
  {
    g_autoptr (GError) err = NULL;
    int length = -1;
    pass = one_pass_listing (c, src, info, indices, &length, &err);
    if (err)
      {
        g_propagate_error (error, g_steal_pointer (&err));
        return FALSE;
      }
    if (pass)
      {
        info = pass;
        g_array_set_size (indices, 0);
        for (guint i = 0; i < pass->titles->len; i++)
          g_array_append_val (indices, ((BroTitle *) pass->titles->pdata[i])->index);
        rip.min_length_seconds = length;
      }
  }
  if (c->rip_info)
    bro_disc_info_unref (c->rip_info);
  c->rip_info = bro_disc_info_ref (info);
  if (c->rip_titles)
    g_array_unref (c->rip_titles);
  c->rip_titles = g_array_ref (indices);
  c->main_title = -1;
  {
    int longest = -1;
    for (guint i = 0; i < indices->len; i++)
      {
        BroTitle *t = bro_disc_info_title (info, g_array_index (indices, int, i));
        if (t && bro_title_duration (t) > longest)
          {
            longest = bro_title_duration (t);
            c->main_title = t->index;
          }
      }
  }
  g_hash_table_remove_all (c->file_titles);
  prepare_episodes (c, src, info, indices);
  if (!check_free_space (c, info, indices, out_dir, error))
    return FALSE;
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
      args = bro_makemkv_mkv_args (c->env, src, tstr, out_dir, &rip);
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
          g_autofree char *why_s = summary_reason (s);
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
  if (media_server (c))
    {
      /* Kept out of the media server's scan (.plexignore for Plex, .ignore for Jellyfin / Emby). */
      g_autofree char *plex = NULL, *ignore = NULL;
      g_free (dest);
      dest = g_build_filename (out_dir, "Backup", NULL);
      g_mkdir_with_parents (dest, 0755);
      plex = g_build_filename (dest, ".plexignore", NULL);
      ignore = g_build_filename (dest, ".ignore", NULL);
      g_file_set_contents (plex, "*\n", -1, NULL);
      g_file_set_contents (ignore, "", 0, NULL);
    }
  else if (in_subfolder)
    {
      g_autoptr (GHashTable) v = base_values (c, BRO_JOB_RUNNING);
      g_autofree char *sub = bro_template_render_path (c->req->drive->output.backup_subfolder, v);
      g_free (dest);
      dest = g_build_filename (out_dir, *sub ? sub : "backup", NULL);
    }
  g_mkdir_with_parents (dest, 0755);
  {
    /* MakeMKV refuses a folder that exists, even an empty one ("already contains a backup"), so it creates it. */
    g_autofree char *templ_name = backup_name (c, decrypt);
    g_autofree char *name = *templ_name ? g_strdup (templ_name)
                                         : bro_sanitize_component (c->res->disc_label && *c->res->disc_label ? c->res->disc_label : "disc");
    g_autofree char *leaf = c->req->drive->rip.backup_format == BRO_BACKUP_ISO ? g_strconcat (name, ".iso", NULL) : g_strdup (name);
    g_autofree char *want = g_build_filename (dest, leaf, NULL);
    g_free (dest);
    dest = bro_unique_path (want);
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
      g_autofree char *why = summary_reason (s);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Backup failed: %s", why);
      summary_free (s);
      return NULL;
    }
  summary_free (s);
  /* MakeMKV writes an ISO image when the destination ends in .iso. If it wrote a folder instead, the backup is
   * complete but isn't an image: keep it as a folder backup rather than failing the job. */
  if (c->req->drive->rip.backup_format == BRO_BACKUP_ISO && g_file_test (dest, G_FILE_TEST_IS_DIR))
    {
      g_autofree char *problem = bro_backup_problem (dest, FALSE);
      if (!problem && g_str_has_suffix (dest, ".iso"))
        {
          g_autofree char *want = g_strndup (dest, strlen (dest) - 4);
          char *folder = bro_unique_path (want);
          if (g_rename (dest, folder) == 0)
            {
              g_autofree char *name = g_path_get_basename (folder);
              log_line (c, BRO_SEV_WARNING, "MakeMKV wrote a folder instead of an ISO image; kept the backup as the folder %s", name);
              g_free (dest);
              dest = folder;
            }
          else
            g_free (folder);
        }
    }
  /* MakeMKV writes DVD backups as ISO images whatever the destination is called: give the image its extension. */
  else if (c->req->drive->rip.backup_format == BRO_BACKUP_FOLDER && g_file_test (dest, G_FILE_TEST_IS_REGULAR))
    {
      g_autofree char *lower = g_ascii_strdown (dest, -1);
      if (!g_str_has_suffix (lower, ".iso"))
        {
          g_autofree char *want = g_strconcat (dest, ".iso", NULL);
          char *image = bro_unique_path (want);
          if (g_rename (dest, image) == 0)
            {
              g_autofree char *name = g_path_get_basename (image);
              log_line (c, BRO_SEV_INFO, "MakeMKV wrote an ISO image (as it does for DVDs); kept the backup as %s", name);
              g_free (dest);
              dest = image;
            }
          else
            g_free (image);
        }
    }
  if (c->req->drive->archive.verify_rips)
    {
      g_autofree char *problem = bro_backup_problem (dest, !g_file_test (dest, G_FILE_TEST_IS_DIR));
      if (problem)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Backup failed the check: %s", problem);
          return NULL;
        }
    }
  log_line (c, BRO_SEV_INFO, "Backup saved to %s", dest);
  /* Without a disc listing a UHD disc looks like a plain Blu-ray; the backup's index.bdmv tells them apart. */
  if (g_file_test (dest, G_FILE_TEST_IS_DIR))
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

/* Reserves the output folder and creates the hidden staging folder inside it, where everything is written until the
 * job has finished and passed its checks. Returns the staging folder. */
static char *
prepare_output (Ctx *c, GError **error)
{
  g_autofree char *final_dir = resolve_output_dir (c, error);
  g_autofree char *short_id = NULL, *name = NULL;
  char *out_dir;
  if (!final_dir)
    return NULL;
  c->output_dir = g_strdup (final_dir);
  c->res->output_dir = g_strdup (final_dir);
  update (c, BRO_UPDATE_OUTPUT_DIR, BRO_SEV_INFO, final_dir, 0, 0);
  short_id = g_ascii_strdown (c->req->job_id, MIN (strlen (c->req->job_id), 8));
  name = g_strconcat (BRO_STAGING_PREFIX, short_id, NULL);
  out_dir = g_build_filename (final_dir, name, NULL);
  c->work_dir = g_strdup (out_dir);
  if (g_mkdir_with_parents (out_dir, 0755) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create %s", out_dir);
      g_free (out_dir);
      return NULL;
    }
  log_line (c, BRO_SEV_INFO, "Output folder: %s (files are moved there once the job has finished and been checked)", final_dir);
  return out_dir;
}

static void
use_match (Ctx *c, BroDiscInfo *info, BroMediaMatch *match, const char *how)
{
  g_autofree char *label = bro_media_match_label (match);
  g_autofree char *tmdb = match->tmdb_id ? g_strdup_printf (", TMDb %d", match->tmdb_id) : g_strdup ("");
  g_autofree char *imdb = match->imdb_id ? g_strdup_printf (", %s", match->imdb_id) : g_strdup ("");
  log_line (c, BRO_SEV_INFO, "%s: “%s”%s%s (%s)", match->provider, label, tmdb, imdb, how);
  bro_media_match_free (c->metadata);
  c->metadata = match;
  resolve_identity (c, info, 0, -1);
}

/* The canonical title and year from TMDb or OMDb, when a provider is set up: the movie or show chosen for this disc
 * (online_id), else the best search result for the name (with the year typed for the disc). Failures only log. */
static void
look_up_metadata (Ctx *c, BroDiscInfo *info)
{
  const BroMetadataConfig *m = &c->req->config->metadata;
  g_autoptr (GError) err = NULL;
  g_autofree char *name = NULL, *query = NULL, *what = NULL, *chosen = NULL;
  const char *provider = m->provider == BRO_METADATA_TMDB ? "TMDb" : "OMDb";
  g_autoptr (GPtrArray) list = NULL;
  int year = 0;
  if (m->provider == BRO_METADATA_NONE || !m->api_key || !*m->api_key || !c->identity || !*c->identity->name)
    return;
  c->looked_up_kind = c->identity->kind;
  name = g_strdup (c->identity->name);
  chosen = g_strstrip (g_strdup (c->req->online_id ? c->req->online_id : ""));
  if (*chosen)
    {
      int tmdb, kind;
      g_autofree char *imdb = NULL;
      if (bro_online_id_parse (chosen, &tmdb, &kind, &imdb))
        {
          g_autofree char *label = bro_online_id_label (tmdb, kind, imdb);
          BroMediaMatch *match;
          what = g_strdup_printf ("Looking up %s", label);
          phase (c, what);
          match = bro_metadata_lookup_id (tmdb, kind, imdb, c->identity->kind, m, &err);
          if (match)
            {
              c->metadata_chosen = TRUE;
              use_match (c, info, match, "chosen for this disc");
            }
          else if (err)
            log_line (c, BRO_SEV_WARNING, "Looking up %s failed: %s", label, err->message);
          else
            log_line (c, BRO_SEV_WARNING, "%s found nothing for %s, chosen for this disc", provider, label);
          return;
        }
      log_line (c, BRO_SEV_WARNING, "“%s” is not a TMDb or IMDb id; looking up the name instead", chosen);
    }
  query = bro_metadata_split_year (name, &year);
  if (c->req->media_year > 0)
    year = c->req->media_year;
  what = g_strdup_printf ("Looking up “%s”", query);
  phase (c, what);
  list = bro_metadata_search (query, c->identity->kind, year, m, &err);
  if (!list)
    {
      log_line (c, BRO_SEV_WARNING, "Looking up “%s” failed: %s", name, err ? err->message : "unknown error");
      return;
    }
  if (!list->len)
    {
      g_autofree char *y = year ? g_strdup_printf (" (%d)", year) : g_strdup ("");
      log_line (c, BRO_SEV_WARNING, "%s found nothing for “%s”%s", provider, query, y);
      return;
    }
  if (list->len == 1)
    use_match (c, info, g_ptr_array_steal_index (list, 0), "the only result");
  else
    {
      GString *how = g_string_new (NULL);
      g_string_printf (how, "best of %u results; also ", list->len);
      for (guint i = 1; i < list->len && i < 5; i++)
        {
          g_autofree char *l = bro_media_match_label (list->pdata[i]);
          g_string_append_printf (how, "%s%s", i > 1 ? ", " : "", l);
        }
      g_string_append_printf (how, "%s. Choose another on the disc page if this is wrong", list->len > 5 ? ", …" : "");
      use_match (c, info, g_ptr_array_steal_index (list, 0), how->str);
      g_string_free (how, TRUE);
    }
}

/* Before an automatic rip, waits for the system to mount the disc (up to automation.waitForMountSeconds). */
static gboolean
wait_for_mount (Ctx *c, GError **error)
{
  int seconds = c->req->drive->automation.wait_for_mount_seconds;
  gint64 deadline;
  gboolean mounted;
  if (!c->req->automatic || c->req->source->kind != BRO_SOURCE_DRIVE || !c->req->source->path || !*c->req->source->path || seconds <= 0)
    return TRUE;
  phase (c, "Waiting for the disc to be mounted");
  deadline = g_get_monotonic_time () + (gint64) seconds * G_USEC_PER_SEC;
  while (!(mounted = bro_disc_is_mounted (c->req->source->path)) && g_get_monotonic_time () < deadline && !cancelled (c))
    g_usleep (G_USEC_PER_SEC / 2);
  if (cancelled (c))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return FALSE;
    }
  if (mounted)
    log_line (c, BRO_SEV_INFO, "The disc is mounted");
  else
    log_line (c, BRO_SEV_INFO, "The disc wasn't mounted after %d s; continuing", seconds);
  return TRUE;
}

static gboolean
on_copy_progress (gint64 done, gint64 total, gpointer data)
{
  Ctx *c = data;
  g_autofree char *text = bro_format_bytes (done);
  gint64 now = g_get_monotonic_time ();
  static __thread gint64 last;
  if (now - last > G_USEC_PER_SEC / 4)
    {
      last = now;
      update (c, BRO_UPDATE_OPERATION, BRO_SEV_INFO, text, 0, 0);
      if (total > 0)
        update (c, BRO_UPDATE_PROGRESS, BRO_SEV_INFO, NULL, (double) done / total, (double) done / total);
    }
  return !cancelled (c);
}

static void
on_audio_line (const char *line, gpointer data)
{
  log_line (data, BRO_SEV_INFO, "%s", line);
}

/* Audio CDs (ripped with cyanrip / abcde, which look the album up in MusicBrainz) and data discs (copied byte for byte
 * to an ISO image). */
static gboolean
execute_other_disc (Ctx *c, GError **error)
{
  BroRunRequest *req = c->req;
  g_autofree char *work = NULL;
  const char *device = req->source->path;

  if (req->source->kind != BRO_SOURCE_DRIVE || !device || !*device)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s needs a disc in a drive", bro_rip_mode_label (req->mode));
      return FALSE;
    }
  resolve_identity (c, NULL, 0, -1);
  if (!(work = prepare_output (c, error)))
    return FALSE;
  steps (c, 0, 1);
  if (req->mode == BRO_MODE_DATA_IMAGE)
    {
      g_autofree char *name = backup_name (c, TRUE);
      g_autofree char *file = g_strconcat (*name ? name : "disc", ".iso", NULL);
      g_autofree char *dest = g_build_filename (work, file, NULL);
      g_autoptr (GError) err = NULL;
      g_autofree char *problem = NULL;
      phase (c, "Copying the disc to an ISO image");
      log_line (c, BRO_SEV_INFO, "Copying %s to %s", device, file);
      if (!bro_copy_disc (device, dest, on_copy_progress, c, req->cancellable, &err))
        {
          if (cancelled (c) || g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          else
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read the disc: %s", err ? err->message : "unknown error");
          return FALSE;
        }
      g_ptr_array_add (c->res->files, g_strdup (dest));
      if (req->drive->archive.verify_rips && (problem = bro_backup_problem (dest, TRUE)))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "The disc image failed the check: %s", problem);
          return FALSE;
        }
      return TRUE;
    }
  {
    g_autoptr (GPtrArray) argv = bro_audio_command (&req->drive->other, device);
    g_autoptr (GPtrArray) items = NULL;
    g_autoptr (GError) err = NULL;
    g_autofree char *cmd = NULL, *tool = NULL;
    BroRunOptions opt = { 0, MAX (0, req->config->stall_timeout_minutes) * 60, 0 };
    BroRunStatus st = { 0 };
    if (!argv)
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "Ripping audio CDs needs cyanrip or abcde (both look up the album in MusicBrainz). "
                             "Install one, or set an audio CD command.");
        return FALSE;
      }
    tool = g_path_get_basename (argv->pdata[0]);
    g_ptr_array_add (argv, NULL);
    cmd = bro_command_line ((const char *const *) argv->pdata);
    phase (c, "Ripping the audio CD");
    update (c, BRO_UPDATE_COMMAND, BRO_SEV_INFO, cmd, 0, 0);
    log_line (c, BRO_SEV_INFO, "$ %s", cmd);
    if (!bro_process_run_ex ((const char *const *) argv->pdata, NULL, work, &opt, req->cancellable, on_audio_line, c, &st, &err))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not start %s: %s", tool, err ? err->message : "unknown error");
        return FALSE;
      }
    if (cancelled (c) || st.cancelled)
      {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
        return FALSE;
      }
    if (st.stalled)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "%s printed nothing for %d minutes and was stopped", tool,
                     req->config->stall_timeout_minutes);
        return FALSE;
      }
    if (st.exit_status != 0)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s failed (exit status %d)", tool, st.exit_status);
        return FALSE;
      }
    items = visible_items (work);
    for (guint i = 0; i < items->len; i++)
      g_ptr_array_add (c->res->files, g_build_filename (work, items->pdata[i], NULL));
    if (!items->len)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s saved nothing", tool);
        return FALSE;
      }
    return TRUE;
  }
}

/* A disc archived before (same fingerprint, in the history or a bromelia.json under the output root): automatic rips
 * follow automation.alreadyArchived, manual ones only warn. FALSE (with c->already_archived set) stops the job. */
static gboolean
check_already_archived (Ctx *c)
{
  g_autofree char *root = output_root (c);
  g_autofree char *when = NULL, *where = NULL;
  g_autoptr (BroArchivedMatch) m = NULL;
  BroAlreadyArchived policy = c->req->drive->automation.already_archived;
  phase (c, "Looking for earlier archives of this disc");
  m = bro_find_archived (c->fingerprint, c->req->archived, root);
  if (!m)
    return TRUE;
  if (m->archived_at)
    {
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (m->archived_at);
      when = g_date_time_format (d, "%Y-%m-%d");
    }
  where = g_strdup_printf ("%s%s%s", m->folder, when ? " on " : "", when ? when : "");
  if (!c->req->automatic || policy == BRO_ARCHIVED_RIP_AGAIN)
    {
      log_line (c, BRO_SEV_WARNING, "This disc was archived before, in %s; ripping it again", where);
      return TRUE;
    }
  c->already_archived = policy == BRO_ARCHIVED_SKIP
    ? g_strdup_printf ("Already archived in %s, so it wasn't ripped again. To archive it again, rip it from the drive page.", where)
    : g_strdup_printf ("Already archived in %s. Rip it again from the drive page, or eject it.", where);
  log_line (c, BRO_SEV_WARNING, "%s", c->already_archived);
  return FALSE;
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
  g_autofree char *out_dir = NULL;

  if (!wait_for_mount (c, error))
    return FALSE;
  if (req->mode == BRO_MODE_AUDIO_CD || req->mode == BRO_MODE_DATA_IMAGE)
    return execute_other_disc (c, error);
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
  /* Separate modes for DVDs, Blu-rays and 4K UHD discs (automatic and quick rips). */
  if (req->uses_configured_mode && c->identity)
    {
      BroRipMode m = bro_rip_config_mode_for (&req->drive->rip, bro_format_key (c->identity->format));
      if (m != req->mode)
        {
          if (!info && (bro_rip_mode_makes_mkv (m) && m != BRO_MODE_BACKUP_THEN_MKV))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s needs the disc listing, which couldn't be read", bro_rip_mode_label (m));
              return FALSE;
            }
          log_line (c, BRO_SEV_INFO, "%s: %s", bro_format_label (c->identity->format), bro_rip_mode_label (m));
          req->mode = m;
          c->res->mode = m;
          resolve_identity (c, info, 0, -1);
        }
    }
  c->fingerprint = bro_disc_fingerprint (info);
  c->res->fingerprint = g_strdup (c->fingerprint);
  if (c->fingerprint && req->mode != BRO_MODE_INFO_ONLY && !check_already_archived (c))
    return FALSE;
  look_up_metadata (c, info);

  if (!(out_dir = prepare_output (c, error)))
    return FALSE;

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
        bsrc = bro_source_new_path (g_file_test (dest, G_FILE_TEST_IS_DIR) ? BRO_SOURCE_FOLDER : BRO_SOURCE_ISO, dest);
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
    case BRO_MODE_AUDIO_CD:
    case BRO_MODE_DATA_IMAGE:
      break; /* handled by execute_other_disc */
    }
  return TRUE;
}

/* ---- post-processing ---- */

gboolean
bro_post_step_should_run (const BroPostStep *step, BroJobState status)
{
  if (!step->enabled || (step->kind == BRO_STEP_COMMAND && (!step->executable || !*g_strstrip (step->executable))))
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
  g_autofree char *interp_raw = g_strstrip (g_strdup (step->interpreter ? step->interpreter : ""));
  g_autofree char *interp = expand_home (interp_raw);

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

const char *const bro_handbrake_presets[] = { "H.265 MKV 1080p30", "H.265 MKV 2160p60 4K", "H.264 MKV 1080p30", "H.264 MKV 480p30",
                                               "Fast 1080p30", "HQ 1080p30 Surround", "Super HQ 1080p30 Surround", "Fast 2160p60 4K HEVC",
                                               NULL };

char *
bro_handbrake_tool (const BroPostStep *step)
{
  g_autofree char *set = g_strstrip (g_strdup (step->executable ? step->executable : ""));
  if (*set)
    {
      char *exe = expand_home (set);
      if (g_file_test (exe, G_FILE_TEST_IS_EXECUTABLE) && !g_file_test (exe, G_FILE_TEST_IS_DIR))
        return exe;
      g_free (exe);
      return NULL;
    }
  return find_tool ("HandBrakeCLI", NULL);
}

gboolean
bro_handbrake_is_source (const char *file)
{
  g_autofree char *lower = g_ascii_strdown (file, -1);
  return g_str_has_suffix (lower, ".mkv") && !g_file_test (file, G_FILE_TEST_IS_DIR);
}

char *
bro_handbrake_output (const BroPostStep *step, GHashTable *values)
{
  g_autofree char *t = g_strstrip (g_strdup (step->output_path ? step->output_path : ""));
  g_autofree char *r = bro_template_render (*t ? t : BRO_HANDBRAKE_DEFAULT_OUTPUT, values, FALSE);
  return expand_home (r);
}

GPtrArray *
bro_handbrake_arguments (const BroPostStep *step, const char *input, const char *output)
{
  GPtrArray *args = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *file_t = g_strstrip (g_strdup (step->preset_file ? step->preset_file : ""));
  g_autofree char *preset = g_strstrip (g_strdup (step->preset ? step->preset : ""));
  g_autoptr (GPtrArray) extra = bro_split_arguments (step->extra_arguments ? step->extra_arguments : "");
  if (*file_t)
    {
      g_ptr_array_add (args, g_strdup ("--preset-import-file"));
      g_ptr_array_add (args, expand_home (file_t));
    }
  if (*preset)
    {
      g_ptr_array_add (args, g_strdup ("--preset"));
      g_ptr_array_add (args, g_strdup (preset));
    }
  g_ptr_array_add (args, g_strdup ("-i"));
  g_ptr_array_add (args, g_strdup (input));
  g_ptr_array_add (args, g_strdup ("-o"));
  g_ptr_array_add (args, g_strdup (output));
  for (guint i = 0; i < extra->len; i++)
    g_ptr_array_add (args, g_strdup (extra->pdata[i]));
  return args;
}

gboolean
bro_handbrake_keep_line (BroProgressFilter *f, const char *line)
{
  const char *at = strstr (line, "Encoding: task ");
  int task = 0, percent = 0;
  if (!at || sscanf (at, "Encoding: task %d of %*d, %d.", &task, &percent) != 2)
    return TRUE;
  if (task != f->task)
    {
      f->task = task;
      f->last = -1;
    }
  if (percent / 10 <= f->last)
    return FALSE;
  f->last = percent / 10;
  return TRUE;
}

typedef void (*StepLogFunc) (BroSeverity sev, const char *text, gpointer data);

typedef struct {
  StepLogFunc log;
  gpointer data;
  const char *name;
  BroProgressFilter *filter; /* HandBrake progress, or NULL */
} StepLog;

static void
step_log (StepLog *sl, BroSeverity sev, const char *fmt, ...) G_GNUC_PRINTF (3, 4);

static void
step_log (StepLog *sl, BroSeverity sev, const char *fmt, ...)
{
  va_list ap;
  g_autofree char *text = NULL;
  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  sl->log (sev, text, sl->data);
}

static void
on_step_line (const char *line, gpointer data)
{
  StepLog *sl = data;
  if (sl->filter)
    {
      /* HandBrakeCLI ends its progress lines with \r. */
      g_auto (GStrv) parts = g_strsplit (line, "\r", -1);
      for (int i = 0; parts[i]; i++)
        if (*parts[i] && bro_handbrake_keep_line (sl->filter, parts[i]))
          step_log (sl, BRO_SEV_INFO, "  [%s] %s", sl->name, parts[i]);
      return;
    }
  step_log (sl, BRO_SEV_INFO, "  [%s] %s", sl->name, line);
}

static GHashTable *copy_values (GHashTable *v);

/* Encodes every MKV of files with HandBrakeCLI (a HandBrake step). Returns whether an encode failed. */
static gboolean
run_handbrake (BroPostStep *step, GHashTable *base_vals, char **base_env, GPtrArray *files, GCancellable *cancellable, StepLogFunc log,
               gpointer log_data)
{
  StepLog sl = { log, log_data, step->name, NULL };
  g_autofree char *tool = bro_handbrake_tool (step);
  gboolean failed = FALSE, any = FALSE;
  BroSeverity bad = step->fail_job_on_error ? BRO_SEV_ERROR : BRO_SEV_WARNING;
  if (!tool)
    {
      step_log (&sl, bad, "%s: HandBrakeCLI was not found. Install HandBrake's command line version (handbrake-cli), or set its location "
                "in the step.", step->name);
      return TRUE;
    }
  for (guint t = 0; t < files->len; t++)
    {
      const char *file = files->pdata[t];
      g_autoptr (GHashTable) values = NULL;
      g_auto (GStrv) envp = NULL;
      g_autoptr (GPtrArray) argv = NULL, args = NULL;
      g_autoptr (GError) err = NULL;
      g_autofree char *fname = NULL, *stem = NULL, *wanted = NULL, *output = NULL, *dir = NULL, *cmd = NULL, *oname = NULL;
      BroProgressFilter filter = { -1, -1 };
      int exit_status = -1;
      gboolean timed_out = FALSE;
      GHashTableIter it;
      gpointer k, v;
      if (!bro_handbrake_is_source (file))
        continue;
      if (g_cancellable_is_cancelled (cancellable))
        return failed;
      any = TRUE;
      values = copy_values (base_vals);
      envp = g_environ_setenv (g_strdupv (base_env), "BROMELIA_FILE", file, TRUE);
      fname = g_path_get_basename (file);
      stem = g_strdup (fname);
      if (strrchr (stem, '.')) *strrchr (stem, '.') = '\0';
      bro_template_values_set (values, "file", file);
      bro_template_values_set (values, "filename", fname);
      bro_template_values_set (values, "stem", stem);
      g_hash_table_iter_init (&it, step->environment);
      while (g_hash_table_iter_next (&it, &k, &v))
        {
          g_autofree char *rv = bro_template_render (v, values, FALSE);
          envp = g_environ_setenv (envp, k, rv, TRUE);
        }
      wanted = bro_handbrake_output (step, values);
      output = bro_unique_path (wanted);
      dir = g_path_get_dirname (output);
      oname = g_path_get_basename (output);
      if (g_mkdir_with_parents (dir, 0755) != 0)
        {
          step_log (&sl, bad, "%s: can't create %s: %s", step->name, dir, g_strerror (errno));
          failed = TRUE;
          continue;
        }
      args = bro_handbrake_arguments (step, file, output);
      argv = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (argv, g_strdup (tool));
      for (guint i = 0; i < args->len; i++)
        g_ptr_array_add (argv, g_strdup (args->pdata[i]));
      g_ptr_array_add (argv, NULL);
      cmd = bro_command_line ((const char *const *) argv->pdata);
      sl.filter = &filter;
      step_log (&sl, BRO_SEV_INFO, "▶ %s (%s → %s): %s", step->name, fname, oname, cmd);
      if (!bro_process_run ((const char *const *) argv->pdata, (const char *const *) envp, dir, step->timeout_seconds, cancellable,
                            on_step_line, &sl, &exit_status, NULL, &timed_out, &err))
        {
          step_log (&sl, bad, "%s could not be started: %s", step->name, err ? err->message : "unknown error");
          failed = TRUE;
          continue;
        }
      if (timed_out)
        step_log (&sl, BRO_SEV_WARNING, "%s (%s) timed out after %d s", step->name, fname, step->timeout_seconds);
      else
        step_log (&sl, exit_status == 0 ? BRO_SEV_INFO : bad, "%s (%s) exited with status %d", step->name, fname, exit_status);
      if (exit_status != 0 || timed_out)
        failed = TRUE;
    }
  if (!any)
    step_log (&sl, BRO_SEV_INFO, "%s: no MKV files to transcode", step->name);
  return failed;
}

static GHashTable *
copy_values (GHashTable *v)
{
  GHashTable *out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GHashTableIter it;
  gpointer k, val;
  g_hash_table_iter_init (&it, v);
  while (g_hash_table_iter_next (&it, &k, &val))
    g_hash_table_insert (out, g_strdup (k), g_strdup (val));
  return out;
}

/* Runs steps (already matched to the disc) for a job that ended with status. Returns whether a step marked "fail the
 * job" failed; *any_failed (optional) receives the first step that failed at all. */
static gboolean
run_steps (GPtrArray *steps_arr, GHashTable *base_vals, char **base_env, GPtrArray *files, const char *output_dir,
           BroJobState status, GCancellable *cancellable, StepLogFunc log, gpointer log_data, int *ran, char **any_failed)
{
  gboolean failed = FALSE;
  for (guint i = 0; i < steps_arr->len; i++)
    {
      BroPostStep *step = steps_arr->pdata[i];
      guint targets = step->per_file ? files->len : 1;
      if (!bro_post_step_should_run (step, status))
        continue;
      if (step->kind == BRO_STEP_HANDBRAKE)
        {
          if (ran)
            (*ran)++;
          if (g_cancellable_is_cancelled (cancellable))
            return failed;
          if (run_handbrake (step, base_vals, base_env, files, cancellable, log, log_data))
            {
              if (step->fail_job_on_error)
                failed = TRUE;
              if (any_failed && !*any_failed)
                *any_failed = g_strdup (step->name);
            }
          continue;
        }
      if (!targets)
        continue;
      if (ran)
        (*ran)++;
      for (guint t = 0; t < targets; t++)
        {
          const char *file = step->per_file ? files->pdata[t] : NULL;
          g_autoptr (GHashTable) values = copy_values (base_vals);
          g_auto (GStrv) envp = g_strdupv (base_env);
          g_autoptr (GPtrArray) argv = NULL;
          g_autoptr (GError) err = NULL;
          g_autofree char *cmd = NULL;
          g_autofree char *wd = NULL;
          int exit_status = -1;
          gboolean timed_out = FALSE;
          GHashTableIter it;
          gpointer k, v;
          StepLog sl = { log, log_data, step->name, NULL };

          if (g_cancellable_is_cancelled (cancellable))
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
          argv = bro_post_step_argv (step, values, files);
          g_ptr_array_add (argv, NULL);
          cmd = bro_command_line ((const char *const *) argv->pdata);
          if (step->working_directory && *step->working_directory)
            {
              g_autofree char *r = bro_template_render (step->working_directory, values, FALSE);
              wd = expand_home (r);
            }
          else
            wd = g_strdup (output_dir);
          step_log (&sl, BRO_SEV_INFO, "▶ %s: %s", step->name, cmd);
          if (!bro_process_run ((const char *const *) argv->pdata, (const char *const *) envp, wd, step->timeout_seconds,
                                cancellable, on_step_line, &sl, &exit_status, NULL, &timed_out, &err))
            {
              step_log (&sl, step->fail_job_on_error ? BRO_SEV_ERROR : BRO_SEV_WARNING, "%s could not be started: %s",
                        step->name, err ? err->message : "unknown error");
              if (step->fail_job_on_error)
                failed = TRUE;
              if (any_failed && !*any_failed)
                *any_failed = g_strdup (step->name);
              continue;
            }
          if (timed_out)
            step_log (&sl, BRO_SEV_WARNING, "%s timed out after %d s", step->name, step->timeout_seconds);
          else
            step_log (&sl, exit_status == 0 ? BRO_SEV_INFO : (step->fail_job_on_error ? BRO_SEV_ERROR : BRO_SEV_WARNING),
                      "%s exited with status %d", step->name, exit_status);
          if ((exit_status != 0 || timed_out) && step->fail_job_on_error)
            failed = TRUE;
          if ((exit_status != 0 || timed_out) && any_failed && !*any_failed)
            *any_failed = g_strdup (step->name);
        }
    }
  return failed;
}

static void
ctx_step_log (BroSeverity sev, const char *text, gpointer data)
{
  log_line (data, sev, "%s", text);
}

/* BROMELIA_* variables for scripts. */
static char **
script_environment (Ctx *c, BroJobState status)
{
  char **base_env = g_get_environ ();
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
  g_string_free (files, TRUE);
  return base_env;
}

/* Runs the steps that apply to this disc; steps marked "background" are handed to the background queue instead
 * (res->background). Returns whether a step marked "fail the job" failed. */
static gboolean
run_post_processing (Ctx *c, BroJobState status)
{
  g_autoptr (GPtrArray) now = g_ptr_array_new ();
  g_autoptr (GPtrArray) later = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_post_step_free);
  g_autofree char *code = c->identity ? bro_identity_format_code (c->identity) : g_strdup ("");
  g_auto (GStrv) base_env = NULL;
  g_autoptr (GHashTable) values = NULL;
  gboolean failed = FALSE;

  /* The drive's steps, then the global plugins; each limited by its name / format conditions. */
  for (int pass = 0; pass < 2; pass++)
    {
      GPtrArray *src = pass == 0 ? c->req->drive->post_process : c->req->config->plugins;
      for (guint i = 0; i < src->len; i++)
        {
          BroPostStep *step = src->pdata[i];
          if (!bro_plugin_matches (step, c->identity ? c->identity->name : c->res->disc_label, c->res->disc_label, code))
            continue;
          if (step->background)
            {
              if (bro_post_step_should_run (step, status))
                g_ptr_array_add (later, bro_post_step_copy (step));
            }
          else
            g_ptr_array_add (now, step);
        }
    }
  if (!now->len && !later->len)
    return FALSE;
  base_env = script_environment (c, status);
  values = base_values (c, status);
  if (now->len)
    {
      phase (c, "Post-processing");
      failed = run_steps (now, values, base_env, c->res->files, c->res->output_dir, status, c->req->cancellable,
                          ctx_step_log, c, NULL, NULL);
    }
  if (later->len && !cancelled (c))
    {
      BroBackgroundWork *w = g_new0 (BroBackgroundWork, 1);
      g_autofree char *title = g_strdup (c->identity && *c->identity->name ? c->identity->name
                                         : (c->res->disc_label && *c->res->disc_label ? c->res->disc_label : "Disc"));
      w->job_id = g_strdup (c->req->job_id);
      w->title = g_steal_pointer (&title);
      w->steps = g_steal_pointer (&later);
      w->values = g_steal_pointer (&values);
      w->environ = g_steal_pointer (&base_env);
      w->files = g_ptr_array_new_with_free_func (g_free);
      for (guint i = 0; i < c->res->files->len; i++)
        g_ptr_array_add (w->files, g_strdup (c->res->files->pdata[i]));
      w->output_dir = g_strdup (c->res->output_dir);
      w->log_file = g_build_filename (c->req->job_dir, "background.txt", NULL);
      w->status = status;
      c->res->background = w;
      log_line (c, BRO_SEV_INFO, "%u post-processing step(s) queued to run in the background", w->steps->len);
    }
  return failed;
}

void
bro_background_work_free (BroBackgroundWork *w)
{
  if (!w)
    return;
  g_free (w->job_id);
  g_free (w->title);
  g_ptr_array_unref (w->steps);
  g_hash_table_unref (w->values);
  g_strfreev (w->environ);
  g_ptr_array_unref (w->files);
  g_free (w->output_dir);
  g_free (w->log_file);
  g_free (w);
}

static void
file_step_log (BroSeverity sev, const char *text, gpointer data)
{
  if (data)
    {
      if (sev == BRO_SEV_INFO)
        fprintf (data, "%s\n", text);
      else
        fprintf (data, "[%s] %s\n", bro_severity_name (sev), text);
      fflush (data);
    }
}

char *
bro_background_work_run (BroBackgroundWork *w, int *ran, GCancellable *cancellable)
{
  FILE *log = fopen (w->log_file, "a");
  char *failed = NULL;
  int count = 0;
  run_steps (w->steps, w->values, w->environ, w->files, w->output_dir, w->status, cancellable, file_step_log, log, &count, &failed);
  if (log)
    fclose (log);
  if (ran)
    *ran = count;
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
  for (guint i = 0; c->rip_titles && (c->rip_info || c->res->info) && i < c->rip_titles->len; i++)
    {
      BroTitle *t = bro_disc_info_title (c->rip_info ? c->rip_info : c->res->info, g_array_index (c->rip_titles, int, i));
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

void
bro_notification_text (BroJobState status, BroRipMode mode, const char *what, guint file_count, const char *output_dir,
                       const char *error, char **title, char **body)
{
  g_autofree char *label = g_ascii_strdown (bro_job_state_label (status), -1);
  *title = g_strdup_printf ("%s %s: %s", bro_rip_mode_short (mode), status == BRO_JOB_SUCCEEDED ? "finished" : label, what);
  switch (status)
    {
    case BRO_JOB_SUCCEEDED:
      *body = g_strdup_printf ("%u item(s) saved to %s", file_count, output_dir ? output_dir : "");
      break;
    case BRO_JOB_COMPLETED_WITH_ERRORS:
      *body = g_strdup_printf ("Read errors: the files were kept in %s", output_dir ? output_dir : "");
      break;
    case BRO_JOB_CANCELLED:
      *body = g_strdup (error && *error ? error : "Cancelled");
      break;
    default:
      *body = g_strdup (error && *error ? error : "Failed");
    }
}

static void
notification_log (const char *text, gpointer data)
{
  log_line (data, BRO_SEV_WARNING, "%s", text);
}

BroRunResult *
bro_run_job (BroRunRequest *req)
{
  Ctx c = { 0 };
  BroRunResult *res = g_new0 (BroRunResult, 1);
  g_autoptr (GError) error = NULL;
  g_autofree char *logpath = NULL;
  BroJobState status = BRO_JOB_SUCCEEDED;
  gboolean executed;

  res->files = g_ptr_array_new_with_free_func (g_free);
  res->disc_label = g_strdup (req->disc_label ? req->disc_label : "");
  res->started_at = g_get_real_time () / G_USEC_PER_SEC;
  c.req = req;
  c.res = res;
  c.commands = g_ptr_array_new_with_free_func (g_free);
  c.separate = g_hash_table_new (g_direct_hash, g_direct_equal);
  c.file_titles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  c.episodes = g_ptr_array_new_with_free_func ((GDestroyNotify) episode_rec_free);
  c.episode_details = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) bro_episode_details_free);
  c.looked_up_kind = -1;
  c.error_messages = g_ptr_array_new_with_free_func (g_free);
  c.data_errors = g_ptr_array_new_with_free_func (g_free);
  c.first_episode = 1;
  c.episode_width = 2;
  c.step_count = 1;
  c.main_title = -1;
  res->mode = req->mode;
  g_mkdir_with_parents (req->job_dir, 0755);
  logpath = g_build_filename (req->job_dir, "log.txt", NULL);
  c.log = fopen (logpath, "a");

  {
    g_autofree char *src = bro_source_display_name (req->source);
    log_line (&c, BRO_SEV_INFO, "Bromelia job %s", req->job_id);
    log_line (&c, BRO_SEV_INFO, "%s from %s using configuration “%s”", bro_rip_mode_label (req->mode), src, req->drive->name);
  }

  executed = execute (&c, &error);
  if (!executed && c.already_archived)
    {
      status = BRO_JOB_CANCELLED;
      res->error = g_strdup (c.already_archived);
    }
  else if (!executed)
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
  /* Name the real cause when MakeMKV itself couldn't work (expired key, LibreDrive required, …). */
  if (status == BRO_JOB_FAILED && c.problem != BRO_NOTICE_NONE)
    {
      char *e = g_strdup_printf ("%s (%s)", bro_notice_explanation (c.problem), res->error ? res->error : "MakeMKV failed");
      g_free (res->error);
      res->error = e;
      log_line (&c, BRO_SEV_ERROR, "%s", bro_notice_explanation (c.problem));
    }
  res->problem = c.problem;
  if (status == BRO_JOB_SUCCEEDED && c.data_errors->len)
    {
      status = BRO_JOB_COMPLETED_WITH_ERRORS;
      res->error = g_strdup_printf ("MakeMKV reported %u read error(s) while reading the disc, so the files may be damaged. "
                                    "They were kept apart from finished archives.", c.data_errors->len);
      log_line (&c, BRO_SEV_ERROR, "%s", res->error);
    }
  status = finish_output (&c, status);
  if (status == BRO_JOB_SUCCEEDED)
    write_media_server_metadata (&c);

  write_manifest (&c, status);
  if (!g_cancellable_is_cancelled (req->cancellable) && !c.already_archived && run_post_processing (&c, status)
      && status == BRO_JOB_SUCCEEDED)
    {
      status = BRO_JOB_FAILED;
      g_free (res->error);
      res->error = g_strdup ("A post-processing step failed");
    }

  if (!req->skip_eject && req->source->kind == BRO_SOURCE_DRIVE &&
      (((status == BRO_JOB_SUCCEEDED || (c.already_archived && req->drive->automation.already_archived == BRO_ARCHIVED_SKIP))
        && req->drive->automation.eject_when_done) ||
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
  res->mode = req->mode;
  if (c.identity && *c.identity->name)
    res->name = g_strdup (c.identity->name);
  write_manifest (&c, status);
  if (req->config->notifications->len)
    {
      g_autofree char *title = NULL, *body = NULL;
      const char *what = res->name ? res->name : (res->disc_label && *res->disc_label ? res->disc_label : "Disc");
      bro_notification_text (status, req->mode, what, res->files->len, res->output_dir, res->error, &title, &body);
      bro_notifications_send (req->config->notifications, title, body, bro_job_state_word (status), notification_log, &c);
    }
  log_line (&c, BRO_SEV_INFO, "Finished: %s", bro_job_state_label (status));
  if (c.log)
    fclose (c.log);
  if (c.makemkv_log)
    fclose (c.makemkv_log);
  /* The logs go wherever the files went, including a folder marked [INCOMPLETE] or [READ ERRORS]. */
  if (req->drive->archive.archive_record && c.files_folder)
    {
      static const char *const logs[][2] = {
        { "log.txt", "bromelia-log.txt" }, { BRO_MAKEMKV_LOG, "makemkv-log.txt" }, { BRO_MAKEMKV_DEBUG_LOG, "makemkv-debug-log.txt" }
      };
      for (guint i = 0; i < G_N_ELEMENTS (logs); i++)
        {
          g_autofree char *from = g_build_filename (req->job_dir, logs[i][0], NULL);
          g_autofree char *want = g_build_filename (c.files_folder, logs[i][1], NULL);
          g_autofree char *target = NULL;
          g_autofree char *contents = NULL;
          gsize len = 0;
          if (!g_file_get_contents (from, &contents, &len, NULL))
            continue;
          target = bro_unique_path (want);
          g_file_set_contents (target, contents, len, NULL);
        }
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
  if (c.rip_info) bro_disc_info_unref (c.rip_info);
  bro_media_match_free (c.metadata);
  g_hash_table_unref (c.episode_details);
  g_free (c.fingerprint);
  g_free (c.already_archived);
  g_free (c.files_folder);
  g_free (c.debug_log);
  return res;
}
