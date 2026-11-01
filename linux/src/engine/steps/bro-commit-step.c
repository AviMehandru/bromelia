/* bro-commit-step.c */
#include "bro-commit-step.h"
#include "bro-commit-output.h"
#include "bro-conflict-namer.h"
#include "bro-directory-entry.h"
#include "bro-naming-settings.h"
#include "bro-disc-request.h"
#include "bro-name-output.h"
#include "bro-plan-output.h"
#include "bro-probe-output.h"
#include "bro-seal-output.h"
#include "bro-step-sources.h"
#include "bro-steps-private.h"
#include <string.h>

#define QUARANTINE_FOLDER "_Quarantine"

struct _BroCommitStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroCommitStep, bro_commit_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_COMMIT;
}

/* Plan §20.1's "rolls forward": a rerun finds the unit's open intent and finishes it. */
static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  BroResourceSet *set = bro_resource_set_new ();
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  const char *writes[] = { plan && plan->library_id && *plan->library_id ? plan->library_id : NULL, NULL };
  set->library_writes = g_strdupv ((char **) writes);
  return set;
}

static BroCommitItem *
item_new (int seq, const char *path, const char *sha256, gboolean is_dir)
{
  BroCommitItem *item = bro_commit_item_new ();
  item->seq = seq;
  item->from_path = g_strdup (path);
  item->to_path = g_strdup (path);
  item->sha256 = g_strdup (sha256);
  item->is_dir = is_dir;
  return item;
}

/* Name's items in order, each in place (a folder when Seal hashed no file of that name), then the record, the logs and
 * the note Seal wrote. Only files carry a SHA-256 (Seal's): Bromelia's own files aren't in SHA256SUMS, and their names
 * hold the unit's id. Into @items. */
static void
add_items (GPtrArray *items, const BroNameOutput *named, const BroSealOutput *seal)
{
  for (guint i = 0; i < named->files->len; i++)
    {
      g_autofree char *path = g_strdup (((BroNamedFile *) named->files->pdata[i])->path);
      gsize n = strlen (path);
      const BroUnitFile *file = NULL;
      while (n > 0 && path[n - 1] == '/')
        path[--n] = '\0';
      for (guint j = 0; j < seal->files->len && !file; j++)
        if (strcmp (((BroUnitFile *) seal->files->pdata[j])->path, path) == 0)
          file = seal->files->pdata[j];
      g_ptr_array_add (items, item_new ((int) items->len, path, file ? file->sha256 : NULL, file == NULL));
    }
  g_ptr_array_add (items, item_new ((int) items->len, seal->record, NULL, FALSE));
  for (guint i = 0; i < seal->logs->len; i++)
    g_ptr_array_add (items, item_new ((int) items->len, seal->logs->pdata[i], NULL, FALSE));
  if (seal->note)
    g_ptr_array_add (items, item_new ((int) items->len, seal->note, NULL, FALSE));
}

/* The archive_units row, from what Seal's record holds: its identity (Name's when it corrected it, else Plan's), the
 * disc's label (the listing's name, else the source's), Probe's fingerprint and MakeMKV version, Seal's status, record
 * and files. */
static BroUnitRecord *
unit_of (const BroJobSnapshot *job, const BroPlanOutput *plan, const BroNameOutput *named, const BroSealOutput *seal, const char *path,
         BroInstant now)
{
  BroJsonValue *probe_json = bro_job_snapshot_output (job, BRO_STEP_KIND_PROBE);
  g_autoptr (BroProbeOutput) probe = probe_json && probe_json->kind == BRO_JSON_VALUE_OBJECT ? bro_probe_output_decode (probe_json) : NULL;
  const BroIdentity *identity = named->identity ? named->identity : plan->identity;
  g_autoptr (BroDiscRequest) request = bro_disc_request_decode (job->request);
  const BroDiscSource *source = request ? request->source : NULL;
  BroUnitRecord *u = bro_unit_record_new ();
  u->id = seal->unit_id;
  u->library_id = g_strdup (plan->library_id);
  u->has_job_id = TRUE;
  u->job_id = job->id;
  u->path = g_strdup (path);
  u->record_file = g_strdup (seal->record);
  u->record_version = 3;
  u->state = BRO_UNIT_STATE_COMMITTING;
  if (!bro_unit_status_from_wire (seal->status, &u->status))
    u->status = BRO_UNIT_STATUS_SUCCESS;
  u->name = g_strdup (identity->name);
  u->kind = identity->kind;
  u->format = g_strdup (bro_disc_format_to_wire (identity->format));
  u->format_code = g_strdup (identity->format_code.text);
  u->encrypted = identity->encrypted;
  u->fingerprint = g_strdup (probe ? probe->fingerprint : NULL);
  u->label = probe && probe->listing && probe->listing->name && *probe->listing->name ? g_strdup (probe->listing->name)
             : source                                                                 ? bro_step_sources_label (source, NULL)
                                                                                      : g_strdup ("");
  u->has_season = identity->label->season >= 0;
  u->season = identity->label->season;
  u->has_part = identity->label->part >= 0;
  u->part = identity->label->part;
  u->has_volume = identity->label->volume >= 0;
  u->volume = identity->label->volume;
  u->has_disc = identity->label->disc >= 0;
  u->disc = identity->label->disc;
  u->makemkv_version = g_strdup (probe && probe->makemkv_version ? probe->makemkv_version : "");
  for (guint i = 0; i < seal->files->len; i++)
    u->bytes += ((BroUnitFile *) seal->files->pdata[i])->size;
  u->file_count = (int) seal->files->len;
  u->attempts = 1;
  u->created_at = now;
  return u;
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  const BroJobSnapshot *job = context->job;
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  g_autoptr (BroSealOutput) seal = bro_seal_output_of (job);
  g_autoptr (BroNameOutput) named = NULL;
  g_autoptr (BroLibraryHandle) library = NULL;
  g_autofree char *staging = NULL;
  g_autofree char *path = NULL;
  g_autofree char *destination = NULL;
  g_autofree char *root_prefix = NULL;
  g_autoptr (BroCommitIntent) intent = NULL;
  g_autoptr (BroUnitRecord) unit = NULL;
  g_autoptr (BroCommitResult) result = NULL;
  g_autoptr (BroCommitOutput) output = NULL;
  BroStepReport *report;
  BroInstant now;
  if (!plan || !seal)
    return _bro_step_skipped ();
  named = bro_name_output_of (job);
  if (!named)
    {
      g_autoptr (BroBroMessage) m = bro_bro_message_new (BRO_MSG_NAME_NO_FOLDER, NULL, BRO_SEVERITY_ERROR);
      *error = bro_bro_message_to_error (m, NULL);
      return NULL;
    }
  library = bro_library_port_require (context->library, plan->library_id, error);
  if (!library)
    return NULL;
  if (bro_cancellation_token_is_cancelled (context->cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return NULL;
    }
  staging = bro_library_port_staging_directory (context->library, plan->library_id, &job->id, error);
  if (!staging)
    return NULL;

  path = seal->tag ? g_strconcat (QUARANTINE_FOLDER "/", named->name, NULL) : g_strdup (named->folder);
  destination = g_strconcat (library->path, "/", path, NULL);
  now = bro_clock_now (context->clock);
  intent = bro_commit_intent_new ();
  intent->unit_id = seal->unit_id;
  intent->job_id = job->id;
  intent->staging = g_strdup (staging);
  intent->destination = g_strdup (destination);
  intent->merge = named->merge && !seal->tag;
  intent->created_at = now;
  add_items (intent->items, named, seal);
  unit = unit_of (job, plan, named, seal, path, now);
  _bro_step_progress (context, 0, BRO_MSG_ARCHIVE_MOVING);
  if (seal->tag)
    result = bro_library_port_quarantine (context->library, unit, intent, seal->files, seal->tag, context->cancel, error);
  else
    {
      /* A folder of its own that another job committed after Name ran (two discs of the same name at once): the next free
       * name, as Name would have given it, unless the profile says skip (owner H3b-2). The library refuses a taken folder
       * before it writes anything, and an open intent or a unit committed already never gets that far. */
      BroJsonValue *given = bro_json_value_member (plan->profile, "naming");
      g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
      g_autoptr (BroNamingSettings) naming = bro_naming_settings_decode (given ? given : none);
      gboolean free_name = !named->merge && naming->conflict_policy != BRO_CONFLICT_POLICY_SKIP;
      /* Each name the library called taken, the first being Name's: never asked for again, even when the listing shows it
       * under other bytes (a file system that ignores case beyond ASCII, or normalization). */
      g_autoptr (GPtrArray) refused_names = g_ptr_array_new_with_free_func (g_free);
      for (;;)
        {
          g_autoptr (BroBroError) refused = NULL;
          const char *slash;
          g_autofree char *parent = NULL;
          g_autofree char *dir = NULL;
          g_autofree char *next = NULL;
          g_autoptr (GPtrArray) entries = NULL;
          g_autoptr (GPtrArray) names = NULL;
          if ((result = bro_library_port_commit (context->library, unit, intent, seal->files, context->cancel, &refused)))
            break;
          if (!free_name || g_strcmp0 (refused->code, bro_message_code_wire (BRO_MSG_LIBRARY_FOLDER_EXISTS)) != 0
              || g_strcmp0 (bro_json_value_get_string (bro_json_value_member (refused->params, "path"), NULL), intent->destination) != 0)
            {
              if (error)
                *error = g_steal_pointer (&refused);
              return NULL;
            }
          slash = strrchr (path, '/');
          parent = slash ? g_strndup (path, slash - path) : g_strdup ("");
          dir = *parent ? g_strconcat (library->path, "/", parent, NULL) : g_strdup (library->path);
          if (!(entries = bro_file_system_list (context->fs, dir, error)))
            return NULL;
          names = g_ptr_array_new ();
          for (guint i = 0; i < entries->len; i++)
            g_ptr_array_add (names, ((BroDirectoryEntry *) entries->pdata[i])->name);
          g_ptr_array_add (refused_names, g_strdup (slash ? slash + 1 : path));
          for (guint i = 0; i < refused_names->len; i++)
            g_ptr_array_add (names, refused_names->pdata[i]);
          g_ptr_array_add (names, NULL);
          next = bro_conflict_namer_next (refused_names->pdata[0], (const char *const *) names->pdata, TRUE);
          g_free (path);
          path = *parent ? g_strconcat (parent, "/", next, NULL) : g_strdup (next);
          g_free (intent->destination);
          intent->destination = g_strconcat (library->path, "/", path, NULL);
          g_free (unit->path);
          unit->path = g_strdup (path);
          _bro_step_log (context, BRO_MSG_ARCHIVE_OUTPUT_FOLDER, _bro_param ("path", bro_json_value_new_string (intent->destination)),
                         BRO_SEVERITY_WARNING);
        }
    }
  if (!result)
    return NULL;

  if (seal->tag)
    _bro_step_log (context, BRO_MSG_COMMIT_QUARANTINED, _bro_param ("path", bro_json_value_new_string (result->destination)), BRO_SEVERITY_WARNING);
  else
    {
      BroJsonValue *params = _bro_param ("count", bro_json_value_new_integer (intent->items->len));
      bro_json_value_set (params, "path", bro_json_value_new_string (result->destination));
      _bro_step_log (context, BRO_MSG_ARCHIVE_MOVED, params, BRO_SEVERITY_INFO);
    }
  root_prefix = g_strconcat (library->path, "/", NULL);
  output = bro_commit_output_new ();
  output->unit_id = seal->unit_id;
  g_free (output->library);
  output->library = g_strdup (plan->library_id);
  g_free (output->path);
  output->path = g_strdup (g_str_has_prefix (result->destination, root_prefix) ? result->destination + strlen (root_prefix) : result->destination);
  output->merged = result->merged;
  output->quarantined = seal->tag != NULL;
  report = bro_step_report_new (bro_commit_output_encode (output));
  report->quarantined = output->quarantined;
  return report;
}

static void
step_iface_init (BroStepInterface *iface)
{
  iface->kind = step_kind;
  iface->resumability = step_resumability;
  iface->resources = step_resources;
  iface->run = step_run;
}

static void
bro_commit_step_class_init (BroCommitStepClass *klass)
{
}

static void
bro_commit_step_init (BroCommitStep *self)
{
}

BroCommitStep *
bro_commit_step_new (void)
{
  return g_object_new (BRO_TYPE_COMMIT_STEP, NULL);
}
