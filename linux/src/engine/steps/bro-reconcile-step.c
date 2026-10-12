/* bro-reconcile-step.c */
#include "bro-reconcile-step.h"
#include "bro-steps-private.h"
#include "bro-step-sources.h"
#include "bro-listing-match.h"
#include "bro-probe-output.h"
#include "bro-reconcile-output.h"
struct _BroReconcileStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroReconcileStep, bro_reconcile_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_RECONCILE;
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

static int
compare_ints (gconstpointer a, gconstpointer b)
{
  int x = GPOINTER_TO_INT (*(gpointer *) a), y = GPOINTER_TO_INT (*(gpointer *) b);
  return x < y ? -1 : x > y;
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  g_autoptr (BroDiscRequest) request = bro_step_sources_request (context->job, error);
  if (!request)
    return NULL;
  if (!request->session)
    return _bro_step_skipped ();
  const BroSessionSnapshot *session = request->session;
  g_autoptr (BroProbeOutput) probe = bro_probe_output_decode (bro_job_snapshot_output (context->job, BRO_STEP_KIND_PROBE));
  if (probe->from_session || !probe->listing)
    return _bro_step_skipped ();
  g_autoptr (BroBroMessage) why = bro_listing_match_different_disc (session->listing, probe->listing);
  if (why)
    return _bro_step_failed (bro_bro_error_new (bro_message_code_wire (BRO_MSG_DISC_CHANGED), _bro_param ("why", bro_bro_message_to_json (why)), NULL));
  g_autoptr (GArray) indices = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GHashTable) same_tracks = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (guint i = 0; i < session->choices->titles->len; i++) {
    const BroTitleChoice *t = session->choices->titles->pdata[i];
    if (t->selected)
      g_array_append_val (indices, t->index);
    if (t->tracks)
      g_hash_table_add (same_tracks, GINT_TO_POINTER (t->index));
  }
  BroBroError *failure = NULL;
  GHashTable *map = bro_listing_match_map_titles (indices, session->listing, probe->listing, same_tracks, &failure);
  if (!map)
    return _bro_step_failed (failure);
  g_autoptr (BroReconcileOutput) output = bro_reconcile_output_new (map);
  g_autoptr (GPtrArray) keys = g_hash_table_get_keys_as_ptr_array (map);
  g_ptr_array_sort (keys, compare_ints);
  BroJsonValue *moved = bro_json_value_new_array ();
  for (guint i = 0; i < keys->len; i++) {
    int from = GPOINTER_TO_INT (keys->pdata[i]), to = GPOINTER_TO_INT (g_hash_table_lookup (map, keys->pdata[i]));
    if (from != to) {
      g_autofree char *change = g_strdup_printf ("%d → %d", from, to);
      bro_json_value_append (moved, bro_json_value_new_string (change));
    }
  }
  if (bro_json_value_length (moved) > 0)
    _bro_step_log (context, BRO_MSG_DISC_TITLE_NUMBERS_CHANGED, _bro_param ("changes", moved), BRO_SEVERITY_INFO);
  else
    bro_json_value_unref (moved);
  return bro_step_report_new (bro_reconcile_output_encode (output));
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
bro_reconcile_step_class_init (BroReconcileStepClass *klass)
{
}

static void
bro_reconcile_step_init (BroReconcileStep *self)
{
}

BroReconcileStep *
bro_reconcile_step_new (void)
{
  return g_object_new (BRO_TYPE_RECONCILE_STEP, NULL);
}
