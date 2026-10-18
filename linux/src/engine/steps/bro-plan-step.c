/* bro-plan-step.c */
#include "bro-plan-step.h"
#include "bro-archive-check.h"
#include "bro-config.h"
#include "bro-identify-step.h"
#include "bro-mode-chooser.h"
#include "bro-one-pass.h"
#include "bro-plan-output.h"
#include "bro-probe-output.h"
#include "bro-profile-resolver.h"
#include "bro-reconcile-output.h"
#include "bro-space-check.h"
#include "bro-space-estimate.h"
#include "bro-step-sources.h"
#include "bro-steps-private.h"
#include "bro-title-rules.h"

#include <string.h>

struct _BroPlanStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroPlanStep, bro_plan_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_PLAN;
}

static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  BroResourceSet *set = bro_resource_set_new ();
  set->drives = g_new0 (char *, 1);
  return set;
}

static BroStepReport *
failed (BroMessageCode code, BroJsonValue *params)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, params, BRO_SEVERITY_ERROR);
  return _bro_step_failed (bro_bro_message_to_error (m, NULL));
}

/* The session's choices as the resolver's session layer: the chosen profile's fields, then the chosen library. NULL
 * without a session (or with nothing chosen). */
static BroJsonValue *
session_layer (const BroConfig *config, const BroSessionChoices *choices)
{
  BroJsonValue *layer;
  if (!choices)
    return NULL;
  layer = bro_json_value_new_object ();
  if (choices->profile)
    {
      g_autoptr (GPtrArray) profiles = bro_config_profiles (config);
      for (guint i = 0; i < profiles->len; i++)
        {
          const BroJsonValue *json = ((BroProfile *) g_ptr_array_index (profiles, i))->json;
          if (g_strcmp0 (bro_json_value_get_string (bro_json_value_member (json, "id"), NULL), choices->profile) != 0
              || json->kind != BRO_JSON_VALUE_OBJECT)
            continue;
          for (guint k = 0; k < json->keys->len; k++)
            {
              const char *key = json->keys->pdata[k];
              if (strcmp (key, "id") != 0 && strcmp (key, "name") != 0 && !(choices->library && strcmp (key, "library") == 0))
                bro_json_value_set (layer, key, bro_json_value_ref (json->items->pdata[k]));
            }
          break;
        }
    }
  if (choices->library)
    bro_json_value_set (layer, "library", bro_json_value_new_string (choices->library));
  if (layer->keys->len == 0)
    g_clear_pointer (&layer, bro_json_value_unref);
  return layer;
}

static char *
profile_name (const BroConfig *config, const char *profile_id)
{
  g_autoptr (GPtrArray) profiles = bro_config_profiles (config);
  for (guint i = 0; i < profiles->len; i++)
    {
      const BroJsonValue *json = ((BroProfile *) g_ptr_array_index (profiles, i))->json;
      const char *name = bro_json_value_get_string (bro_json_value_member (json, "name"), "");
      if (g_strcmp0 (bro_json_value_get_string (bro_json_value_member (json, "id"), NULL), profile_id) == 0 && *name)
        return g_strdup (name);
    }
  return g_strdup (profile_id);
}

static int
by_index (gconstpointer a, gconstpointer b)
{
  const BroTitleChoice *x = *(BroTitleChoice *const *) a, *y = *(BroTitleChoice *const *) b;
  return x->index - y->index;
}

static const BroTitle *
title_of (const BroListing *listing, int index)
{
  for (guint i = 0; i < listing->titles->len; i++)
    if (((BroTitle *) g_ptr_array_index (listing->titles, i))->index == index)
      return g_ptr_array_index (listing->titles, i);
  return NULL;
}

/* The titles to rip, into @plan, with the one-pass length and the space they need; a failed report, or NULL. */
static BroStepReport *
choose_titles (const BroStepContext *context, const BroListing *listing, const BroSessionChoices *choices, gboolean by_hand,
               const BroEffectiveProfile *profile, BroPlanOutput *plan)
{
  g_autoptr (GArray) picked = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GArray) indices = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GPtrArray) chosen = g_ptr_array_new ();
  BroBytes bytes;
  if (by_hand)
    {
      BroJsonValue *reconciled = bro_job_snapshot_output (context->job, BRO_STEP_KIND_RECONCILE);
      g_autoptr (BroReconcileOutput) map = reconciled && reconciled->kind == BRO_JSON_VALUE_OBJECT ? bro_reconcile_output_decode (reconciled) : NULL;
      for (guint i = 0; i < choices->titles->len; i++)
        {
          const BroTitleChoice *t = g_ptr_array_index (choices->titles, i);
          gpointer now;
          BroTitleChoice *copy;
          if (!t->selected)
            continue;
          copy = bro_title_choice_copy (t);
          if (map && g_hash_table_lookup_extended (map->title_map, GINT_TO_POINTER (t->index), NULL, &now))
            copy->index = GPOINTER_TO_INT (now);
          if (title_of (listing, copy->index))
            g_ptr_array_add (plan->titles, copy);
          else
            bro_title_choice_free (copy);
        }
      g_ptr_array_sort (plan->titles, by_index);
      if (plan->titles->len == 0)
        return failed (BRO_MSG_TITLES_NONE_CHOSEN_EXIST, NULL);
    }
  else
    {
      g_autoptr (BroTitleSettings) rules = bro_title_settings_decode (bro_json_value_member (profile->profile, "titles"));
      g_autoptr (BroSelection) selection = bro_title_rules_select (listing, rules);
      if (selection->error)
        return _bro_step_failed (bro_bro_message_to_error (selection->error, NULL));
      if (selection->requires_manual_choice)
        {
          g_autofree char *name = profile_name (context->config, plan->profile_id);
          return failed (BRO_MSG_TITLES_MANUAL_REQUIRED, _bro_param ("profile", bro_json_value_new_string (name)));
        }
      for (guint i = 0; i < selection->trace->len; i++)
        {
          const BroSelectionTrace *trace = g_ptr_array_index (selection->trace, i);
          const BroTitle *t = title_of (listing, trace->index);
          BroJsonValue *params;
          if (!t)
            continue;
          params = _bro_param ("title", bro_json_value_new_integer (t->index));
          bro_json_value_set (params, "duration", bro_json_value_new_string (t->duration));
          bro_json_value_set (params, "chapters", bro_json_value_new_integer (t->chapters));
          if (!trace->selected)
            bro_json_value_set (params, "reason", bro_bro_message_to_json (trace->reason));
          _bro_step_log (context, trace->selected ? BRO_MSG_TITLES_SELECTED : BRO_MSG_TITLES_SKIPPED, params, BRO_SEVERITY_INFO);
        }
      if (selection->indices->len == 0)
        return failed (BRO_MSG_TITLES_NONE_MATCHED, NULL);
      for (guint i = 0; i < selection->indices->len; i++)
        g_ptr_array_add (plan->titles, bro_title_choice_new (g_array_index (selection->indices, int, i), TRUE, NULL, ""));
    }
  for (guint i = 0; i < plan->titles->len; i++)
    {
      const BroTitleChoice *t = g_ptr_array_index (plan->titles, i);
      g_array_append_val (indices, t->index);
      if (t->tracks)
        g_array_append_val (picked, t->index);
    }
  if (picked->len == 0)
    {
      BroJsonValue *min = bro_json_value_member (bro_json_value_member (profile->profile, "makemkv"), "minLengthSeconds");
      BroOnePassPlan pass;
      int current = min && min->kind == BRO_JSON_VALUE_INTEGER ? (int) min->integer : -1;
      if (bro_one_pass_plan (indices, listing, current, &pass))
        {
          plan->has_one_pass_min_length = TRUE;
          plan->one_pass_min_length = pass.min_length;
        }
    }
  for (guint i = 0; i < listing->titles->len; i++)
    {
      BroTitle *t = g_ptr_array_index (listing->titles, i);
      for (guint k = 0; k < indices->len; k++)
        if (g_array_index (indices, int, k) == t->index)
          {
            g_ptr_array_add (chosen, t);
            break;
          }
    }
  bytes = bro_space_estimate_for_plan (chosen, picked, -1);
  if (bytes.count > 0)
    {
      plan->has_space_need = TRUE;
      plan->space_need = bytes.count;
    }
  return NULL;
}

static void
append (GArray *steps, BroStepKind kind)
{
  g_array_append_val (steps, kind);
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  const BroJobSnapshot *job = context->job;
  g_autoptr (BroDiscRequest) request = bro_step_sources_request (job, error);
  if (!request)
    return NULL;
  g_autoptr (BroProbeOutput) probe = bro_probe_output_decode (bro_job_snapshot_output (job, BRO_STEP_KIND_PROBE));
  BroJsonValue *identified = bro_job_snapshot_output (job, BRO_STEP_KIND_IDENTIFY);
  g_autoptr (BroJsonValue) none = bro_json_value_new_object ();
  g_autoptr (BroPlanOutput) plan = bro_plan_output_new ();
  g_autoptr (BroDriveLocation) drive = request->source->kind == BRO_DISC_SOURCE_DRIVE && context->drive_lookup
                                           ? bro_drive_lookup_find (context->drive_lookup, request->source->value)
                                           : NULL;
  const char *config_id = drive ? drive->config_id : NULL;
  const BroSessionChoices *choices = request->session ? request->session->choices : NULL;
  const BroListing *listing = probe->listing;
  g_autofree char *label = listing && *listing->name ? g_strdup (listing->name) : bro_step_sources_label (request->source, drive);
  gboolean by_hand = FALSE;
  BroRuleFacts facts = { NULL };
  g_autoptr (BroJsonValue) layer = session_layer (context->config, choices);
  g_autoptr (BroEffectiveProfile) profile = NULL;
  const char *profile_id = NULL;
  const char *library_id;
  g_autoptr (BroLibraryHandle) library = NULL;
  g_autoptr (BroBroMessage) skip = NULL;
  gboolean decide;
  BroStepReport *report;

  bro_identity_free (plan->identity);
  plan->identity = bro_identity_decode (identified ? identified : none);
  facts.name = plan->identity->name;
  facts.label = label;
  facts.format_code = plan->identity->format_code.text;
  facts.kind = bro_media_kind_to_wire (plan->identity->kind);
  facts.automatic = job->request->automatic;
  profile = bro_profile_resolver_resolve (context->config, config_id, &facts, layer);
  if (choices && choices->profile)
    profile_id = choices->profile;
  for (guint i = 0; i < profile->trace->len && !profile_id; i++)
    {
      const BroResolveTrace *t = g_ptr_array_index (profile->trace, profile->trace->len - 1 - i);
      if (t->layer == BRO_RESOLVE_LAYER_DRIVE_PROFILE)
        profile_id = t->id;
    }
  for (guint i = 0; i < profile->trace->len && !profile_id; i++)
    {
      const BroResolveTrace *t = g_ptr_array_index (profile->trace, i);
      if (t->layer == BRO_RESOLVE_LAYER_DEFAULT_PROFILE)
        profile_id = t->id;
    }
  g_free (plan->profile_id);
  plan->profile_id = g_strdup (profile_id ? profile_id : "default");
  bro_json_value_unref (plan->profile);
  plan->profile = bro_json_value_ref (profile->profile);

  for (guint i = 0; choices && i < choices->titles->len; i++)
    by_hand |= ((BroTitleChoice *) g_ptr_array_index (choices->titles, i))->selected;
  if (!bro_rip_mode_from_wire (bro_json_value_get_string (bro_json_value_member (job->request->options, "mode"), ""), &plan->mode))
    {
      BroDiscFlags flags = { 0 };
      g_autoptr (BroProfile) p = bro_profile_new (profile->profile);
      if (drive && drive->has_flags)
        flags = drive->flags;
      if (by_hand)
        plan->mode = BRO_RIP_MODE_MKV;
      else if (!bro_mode_chooser_mode (plan->identity->format, flags, BRO_DISC_CONTENT_VIDEO, p, FALSE, &plan->mode))
        plan->mode = BRO_RIP_MODE_MKV;
    }
  if ((plan->mode == BRO_RIP_MODE_BACKUP) != plan->identity->encrypted)
    {
      BroIdentity *again = bro_identify_step_resolve (context, plan->mode == BRO_RIP_MODE_BACKUP, error);
      if (!again)
        return NULL;
      bro_identity_free (plan->identity);
      plan->identity = again;
    }

  if (plan->mode == BRO_RIP_MODE_MKV || plan->mode == BRO_RIP_MODE_BACKUP_THEN_MKV)
    {
      BroStepReport *refused;
      if (!listing)
        return failed (BRO_MSG_DISC_NO_INFORMATION, NULL);
      if ((refused = choose_titles (context, listing, choices, by_hand, profile, plan)))
        return refused;
    }

  library_id = bro_json_value_get_string (bro_json_value_member (profile->profile, "library"), NULL);
  if (!library_id)
    library_id = bro_json_value_get_string (bro_json_value_member (context->config->document, "defaultLibrary"), NULL);
  {
    g_autoptr (GPtrArray) libraries = bro_config_libraries (context->config);
    g_free (plan->library_id);
    plan->library_id = g_strdup (library_id ? library_id : libraries->len > 0 ? ((BroLibraryEntry *) g_ptr_array_index (libraries, 0))->id : NULL);
  }
  if (!plan->library_id)
    {
      plan->library_id = g_strdup ("");
      return failed (BRO_MSG_LIBRARY_NONE, NULL);
    }
  {
    g_autoptr (BroBroError) offline = NULL;
    library = bro_library_port_require (context->library, plan->library_id, &offline);
    if (!library && offline && strcmp (offline->code, bro_message_code_wire (BRO_MSG_LIBRARY_NOT_FOUND)) == 0)
      {
        *error = g_steal_pointer (&offline);
        return NULL;
      }
    /* Offline: Decide waits for it. */
  }

  if (library && plan->mode != BRO_RIP_MODE_INFO_ONLY)
    {
      g_autoptr (BroBroError) failure = NULL;
      g_autoptr (BroArchivedMatch) match = bro_archive_check_find (context, probe->fingerprint, &failure);
      if (failure)
        {
          *error = g_steal_pointer (&failure);
          return NULL;
        }
      if (match)
        {
          skip = bro_archive_check_apply (context, match, config_id);
          if (bro_archive_check_asks (context, config_id))
            plan->ask_archived = g_steal_pointer (&match);
        }
    }
  decide = !library || plan->ask_archived;
  if (!skip && !decide && plan->has_space_need)
    {
      if (!bro_space_check_reserve (context, library->path, plan->space_need, error))
        return NULL;
      plan->space_reserved = TRUE;
    }
  plan->library_offline = library == NULL;

  report = bro_step_report_new (bro_plan_output_encode (plan));
  report->plan = bro_job_plan_new ();
  report->plan->mode = g_strdup (bro_rip_mode_to_wire (plan->mode));
  report->plan->profile_id = g_strdup (plan->profile_id);
  report->plan->library_id = g_strdup (plan->library_id);
  if (skip)
    {
      report->skip_job = g_steal_pointer (&skip);
      return report;
    }
  if (decide)
    append (report->append_steps, BRO_STEP_KIND_DECIDE);
  append (report->append_steps, BRO_STEP_KIND_ACQUIRE);
  append (report->append_steps, BRO_STEP_KIND_RELEASE);
  if (plan->mode != BRO_RIP_MODE_INFO_ONLY
      && bro_json_value_get_bool (bro_json_value_member (bro_json_value_member (profile->profile, "archive"), "verifyRips"), TRUE))
    append (report->append_steps, BRO_STEP_KIND_VERIFY_RIPS);
  append (report->append_steps, BRO_STEP_KIND_NAME);
  append (report->append_steps, BRO_STEP_KIND_SEAL);
  append (report->append_steps, BRO_STEP_KIND_COMMIT);
  append (report->append_steps, BRO_STEP_KIND_NOTIFY);
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
bro_plan_step_class_init (BroPlanStepClass *klass)
{
}

static void
bro_plan_step_init (BroPlanStep *self)
{
}

BroPlanStep *
bro_plan_step_new (void)
{
  return g_object_new (BRO_TYPE_PLAN_STEP, NULL);
}
