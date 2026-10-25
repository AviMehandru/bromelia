/* bro-seal-record.c */
#include "bro-seal-record.h"
#include "bro-disc-request.h"
#include "bro-label-parser.h"
#include "bro-log-line-dto.h"
#include "bro-name-output.h"
#include "bro-probe-output.h"
#include "bro-seal-output.h"
#include "bro-step-sources.h"

static BroJsonValue *
string_or_null (const char *s)
{
  return s ? bro_json_value_new_string (s) : bro_json_value_new_null ();
}

static BroJsonValue *
string_or_empty (const char *s)
{
  return bro_json_value_new_string (s ? s : "");
}

/* A place in a set: -1 is none. */
static BroJsonValue *
number_or_null (int n)
{
  return n >= 0 ? bro_json_value_new_integer (n) : bro_json_value_new_null ();
}

/* The job's source as the record names it: drive:<id> (MakeMKV's numbering of a drive doesn't last), or iso:,
 * folder:, file: and the path. */
static char *
source_text (const BroDiscSource *source)
{
  if (!source)
    return g_strdup ("");
  switch (source->kind) {
  case BRO_DISC_SOURCE_DRIVE:
    return g_strconcat ("drive:", source->value, NULL);
  case BRO_DISC_SOURCE_ISO:
    return g_strconcat ("iso:", source->value, NULL);
  case BRO_DISC_SOURCE_FOLDER:
    return g_strconcat ("folder:", source->value, NULL);
  case BRO_DISC_SOURCE_FILE:
    return g_strconcat ("file:", source->value, NULL);
  }
  return g_strdup ("");
}

static const BroTitle *
title_at (const BroListing *listing, int index)
{
  for (guint i = 0; listing && i < listing->titles->len; i++)
    if (((BroTitle *) listing->titles->pdata[i])->index == index)
      return listing->titles->pdata[i];
  return NULL;
}

/* {code, params, text, readError?}; takes @m. */
static BroJsonValue *
problem (BroBroMessage *m, gboolean read_error, BroMessageRenderer *messages)
{
  BroJsonValue *o = bro_json_value_new_object ();
  char *text = bro_message_renderer_render (messages, m);
  bro_json_value_set (o, "code", bro_json_value_new_string (bro_message_code_wire (m->code)));
  bro_json_value_set (o, "params", bro_json_value_ref (m->params));
  bro_json_value_set (o, "text", bro_json_value_new_string (text));
  if (read_error)
    bro_json_value_set (o, "readError", bro_json_value_new_bool (TRUE));
  g_free (text);
  bro_bro_message_free (m);
  return o;
}

static BroJsonValue *
param (const char *key, BroJsonValue *value)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, key, value);
  return o;
}

BroJsonValue *
bro_seal_record_build (const BroJobSnapshot *job, const BroPlanOutput *plan, const BroAcquireOutput *acquired, const BroId *unit_id,
                       GPtrArray *files, GPtrArray *log, GPtrArray *logs, BroInstant finished_at, const char *app_version,
                       BroMessageRenderer *messages, const char *status)
{
  BroJsonValue *probe_json = bro_job_snapshot_output (job, BRO_STEP_KIND_PROBE);
  g_autoptr (BroProbeOutput) probe = probe_json && probe_json->kind == BRO_JSON_VALUE_OBJECT ? bro_probe_output_decode (probe_json) : NULL;
  const BroListing *listing = probe ? probe->listing : NULL;
  g_autoptr (BroNameOutput) named = bro_name_output_of (job);
  const BroIdentity *identity = named && named->identity ? named->identity : plan->identity;
  g_autoptr (BroDiscRequest) request = bro_disc_request_decode (job->request);
  const BroDiscSource *source = request ? request->source : NULL;
  const char *drive_id = source && source->kind == BRO_DISC_SOURCE_DRIVE ? source->value : NULL;
  int read_errors = bro_acquire_output_read_error_count (acquired);
  g_autofree char *started = bro_instant_format (job->has_started_at ? job->started_at : finished_at);
  g_autofree char *finished = bro_instant_format (finished_at);
  g_autofree char *label = listing && listing->name && *listing->name ? g_strdup (listing->name)
                           : source                                  ? bro_step_sources_label (source, NULL)
                                                                     : g_strdup ("");
  g_autofree char *set = bro_label_parser_set_description (identity->label);
  g_autofree char *short_id = g_strndup (unit_id->value, 8);
  g_autofree char *source_name = source_text (source);
  BroJsonValue *doc = bro_json_value_new_object ();
  BroJsonValue *unit = bro_json_value_new_object ();
  BroJsonValue *disc = bro_json_value_new_object ();
  BroJsonValue *acquisition = bro_json_value_new_object ();
  BroJsonValue *job_json = bro_json_value_new_object ();
  BroJsonValue *titles = bro_json_value_new_array ();
  BroJsonValue *file_json = bro_json_value_new_array ();
  BroJsonValue *attempts = bro_json_value_new_array ();
  BroJsonValue *attempt = bro_json_value_new_object ();
  BroJsonValue *attempt_titles = bro_json_value_new_object ();
  BroJsonValue *kept = bro_json_value_new_array ();
  BroJsonValue *counts = bro_json_value_new_object ();
  BroJsonValue *problems = bro_json_value_new_array ();
  BroJsonValue *log_names = bro_json_value_new_array ();
  int warnings = 0, errors = 0;

  bro_json_value_set (unit, "id", bro_json_value_new_string (unit_id->value));
  bro_json_value_set (unit, "short", bro_json_value_new_string (short_id));
  bro_json_value_set (unit, "library", bro_json_value_new_string (plan->library_id));

  bro_json_value_set (disc, "label", bro_json_value_new_string (label));
  bro_json_value_set (disc, "volumeName", string_or_empty (listing ? listing->volume_name : NULL));
  bro_json_value_set (disc, "type", string_or_empty (listing ? listing->type_text : NULL));
  bro_json_value_set (disc, "format", bro_json_value_new_string (bro_disc_format_to_wire (identity->format)));
  bro_json_value_set (disc, "formatCode", bro_json_value_new_string (identity->format_code.text));
  bro_json_value_set (disc, "encrypted", bro_json_value_new_bool (identity->encrypted));
  bro_json_value_set (disc, "season", number_or_null (identity->label->season));
  bro_json_value_set (disc, "part", number_or_null (identity->label->part));
  bro_json_value_set (disc, "volume", number_or_null (identity->label->volume));
  bro_json_value_set (disc, "disc", number_or_null (identity->label->disc));
  bro_json_value_set (disc, "set", bro_json_value_new_string (set));
  bro_json_value_set (disc, "fingerprint", string_or_null (probe ? probe->fingerprint : NULL));
  bro_json_value_set (disc, "libreDrive", string_or_null (probe ? probe->libre_drive : NULL));

  bro_json_value_set (acquisition, "mode", bro_json_value_new_string (bro_rip_mode_to_wire (plan->mode)));
  bro_json_value_set (acquisition, "rip",
                      bro_json_value_new_string (plan->mode == BRO_RIP_MODE_BACKUP || plan->mode == BRO_RIP_MODE_BACKUP_DECRYPTED ? "Backup" : "Rip"));
  bro_json_value_set (acquisition, "source", bro_json_value_new_string (source_name));
  if (drive_id)
    bro_json_value_set (acquisition, "drive", param ("id", bro_json_value_new_string (drive_id)));
  bro_json_value_set (acquisition, "profile", bro_json_value_new_string (plan->profile_id));
  bro_json_value_set (acquisition, "makemkv", string_or_empty (probe ? probe->makemkv_version : NULL));
  bro_json_value_set (acquisition, "bromelia", string_or_empty (app_version));
  bro_json_value_set (acquisition, "tools", bro_json_value_new_object ());

  bro_json_value_set (job_json, "id", bro_json_value_new_string (job->id.value));
  bro_json_value_set (job_json, "startedAt", bro_json_value_new_string (started));
  bro_json_value_set (job_json, "finishedAt", bro_json_value_new_string (finished));

  for (guint i = 0; i < acquired->titles->len; i++)
    {
      const BroAcquiredTitle *t = acquired->titles->pdata[i];
      const BroTitle *listed = title_at (listing, t->index);
      g_autofree char *index = g_strdup_printf ("%d", t->index);
      BroJsonValue *o;
      bro_json_value_set (attempt_titles, index, bro_json_value_new_integer (t->read_errors->len));
      bro_json_value_append (kept, bro_json_value_new_integer (t->index));
      if (!listed)
        continue;
      o = bro_json_value_new_object ();
      bro_json_value_set (o, "index", bro_json_value_new_integer (t->index));
      bro_json_value_set (o, "sourceTitleId", listed->has_source_title_id ? bro_json_value_new_integer (listed->source_title_id) : bro_json_value_new_null ());
      bro_json_value_set (o, "sourceFile", string_or_empty (listed->source_file));
      bro_json_value_set (o, "duration", string_or_empty (listed->duration));
      bro_json_value_set (o, "durationSeconds", bro_json_value_new_integer (listed->duration_seconds));
      bro_json_value_set (o, "chapters", bro_json_value_new_integer (listed->chapters));
      bro_json_value_set (o, "sizeBytes", bro_json_value_new_integer (listed->size_bytes));
      bro_json_value_set (o, "segmentMap", string_or_empty (listed->segment_map));
      bro_json_value_set (o, "readErrors", bro_json_value_new_integer (t->read_errors->len));
      bro_json_value_append (titles, o);
    }

  for (guint i = 0; i < files->len; i++)
    bro_json_value_append (file_json, _bro_seal_output_file (files->pdata[i]));

  bro_json_value_set (attempt, "job", bro_json_value_new_string (job->id.value));
  if (drive_id)
    bro_json_value_set (attempt, "drive", bro_json_value_new_string (drive_id));
  bro_json_value_set (attempt, "startedAt", bro_json_value_new_string (started));
  bro_json_value_set (attempt, "titles", attempt_titles);
  bro_json_value_set (attempt, "kept", kept);
  bro_json_value_append (attempts, attempt);

  for (guint i = 0; i < log->len; i++)
    {
      const BroLogLineDto *l = log->pdata[i];
      warnings += l->severity == BRO_SEVERITY_WARNING;
      errors += l->severity == BRO_SEVERITY_ERROR;
    }
  bro_json_value_set (counts, "warnings", bro_json_value_new_integer (warnings));
  bro_json_value_set (counts, "errors", bro_json_value_new_integer (errors));

  for (guint i = 0; i <= acquired->titles->len; i++)
    {
      GPtrArray *texts = i < acquired->titles->len ? ((BroAcquiredTitle *) acquired->titles->pdata[i])->read_errors : acquired->read_errors;
      for (guint j = 0; j < texts->len; j++)
        bro_json_value_append (problems, problem (bro_bro_message_new (BRO_MSG_MAKEMKV_MESSAGE, param ("text", bro_json_value_new_string (texts->pdata[j])),
                                                                       BRO_SEVERITY_ERROR),
                                                  TRUE, messages));
    }
  if (read_errors > 0)
    bro_json_value_append (problems, problem (bro_bro_message_new (BRO_MSG_RIP_READ_ERRORS, param ("count", bro_json_value_new_integer (read_errors)),
                                                                   BRO_SEVERITY_ERROR),
                                              FALSE, messages));

  for (guint i = 0; i < logs->len; i++)
    bro_json_value_append (log_names, bro_json_value_new_string (logs->pdata[i]));

  bro_json_value_set (doc, "format", bro_json_value_new_string ("bromelia-archive"));
  bro_json_value_set (doc, "version", bro_json_value_new_integer (3));
  bro_json_value_set (doc, "unit", unit);
  bro_json_value_set (doc, "status", bro_json_value_new_string (status ? status : read_errors > 0 ? "errors" : "success"));
  bro_json_value_set (doc, "name", bro_json_value_new_string (identity->name));
  bro_json_value_set (doc, "kind", bro_json_value_new_string (bro_media_kind_to_wire (identity->kind)));
  bro_json_value_set (doc, "disc", disc);
  bro_json_value_set (doc, "acquisition", acquisition);
  bro_json_value_set (doc, "job", job_json);
  bro_json_value_set (doc, "titles", titles);
  bro_json_value_set (doc, "episodes", bro_json_value_new_array ());
  bro_json_value_set (doc, "files", file_json);
  bro_json_value_set (doc, "attempts", attempts);
  bro_json_value_set (doc, "counts", counts);
  bro_json_value_set (doc, "problems", problems);
  bro_json_value_set (doc, "logs", log_names);
  return doc;
}
