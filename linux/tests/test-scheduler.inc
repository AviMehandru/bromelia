/* test-scheduler.inc: shared/fixtures/engine/scheduler.cases.json, on a real executor (included by test-engine.c). */
#include "bro-cancellation-source.h"
#include "bro-message-code.h"
#include "bro-scheduler.h"

typedef struct {
  World w;
  BroEventBus *bus;
  BroScheduler *scheduler;
  BroSubscription *subscription;
  GHashTable *jobs;    /* name → BroId * */
  GPtrArray *granted;  /* char * */
  GHashTable *states;  /* name → char * */
  GHashTable *waits;   /* name → GPtrArray of char * */
  GHashTable *leases;  /* name → BroLease */
  GHashTable *cancels; /* name → BroCancellationSource */
  GPtrArray *events;   /* BroEngineEvent */
  GPtrArray *failures;
  const char *id;
} SchedulerHarness;

static BroSchedulerLimits
limits_of (BroJsonValue *j)
{
  BroSchedulerLimits l;
  l.acquisitions = bro_json_value_get_integer (j ? bro_json_value_member (j, "acquisitions") : NULL, 0);
  l.processing = bro_json_value_get_integer (j ? bro_json_value_member (j, "processing") : NULL, 1);
  l.cpu = bro_json_value_get_integer (j ? bro_json_value_member (j, "cpu") : NULL, 2);
  l.io_per_volume = bro_json_value_get_integer (j ? bro_json_value_member (j, "ioPerVolume") : NULL, 2);
  return l;
}

static GStrv
strv_of (BroJsonValue *j)
{
  GStrvBuilder *b = g_strv_builder_new ();
  GStrv v;
  for (guint i = 0; j && i < bro_json_value_length (j); i++)
    g_strv_builder_add (b, bro_json_value_get_string (bro_json_value_at (j, i), ""));
  v = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return v;
}

static const BroId *
job_of (SchedulerHarness *h, const char *name)
{
  BroId *id = g_hash_table_lookup (h->jobs, name);
  if (!id)
    {
      id = g_new0 (BroId, 1);
      g_snprintf (id->value, sizeof id->value, "00000000-0000-4000-8000-%012x", g_hash_table_size (h->jobs) + 1);
      g_hash_table_insert (h->jobs, g_strdup (name), id);
    }
  return id;
}

static const char *
name_of (SchedulerHarness *h, const BroId *id)
{
  GHashTableIter it;
  gpointer name, value;
  g_hash_table_iter_init (&it, h->jobs);
  while (g_hash_table_iter_next (&it, &name, &value))
    if (strcmp (((BroId *) value)->value, id->value) == 0)
      return name;
  return "?";
}

static void
set_state (SchedulerHarness *h, const char *name, const char *state)
{
  g_hash_table_replace (h->states, g_strdup (name), g_strdup (state));
}

typedef struct {
  SchedulerHarness *h;
  char *name;
} Asker;

static void
asker_free (gpointer data)
{
  Asker *a = data;
  g_free (a->name);
  g_free (a);
}

static void
asker_waits (const BroWait *wait, gpointer data)
{
  Asker *a = data;
  g_autofree char *text = wait->kind == BRO_WAIT_BLOCKED ? g_strconcat ("blocked:", wait->reason->code, NULL) : g_strdup ("waiting");
  g_ptr_array_add (g_hash_table_lookup (a->h->waits, a->name), g_strdup (text));
  set_state (a->h, a->name, text);
}

static void
asker_done (BroLease *lease, const BroBroError *error, gpointer data)
{
  Asker *a = data;
  if (lease)
    {
      g_hash_table_replace (a->h->leases, g_strdup (a->name), lease);
      g_ptr_array_add (a->h->granted, g_strdup_printf ("%012" G_GINT64_FORMAT " %s", lease->seq, a->name));
      set_state (a->h, a->name, "granted");
    }
  else
    set_state (a->h, a->name, strcmp (error->code, "job.cancelled") == 0 ? "cancelled" : "failed");
}

static void
event_seen (BroEngineEvent *e, gpointer data)
{
  g_ptr_array_add (data, bro_engine_event_ref (e));
}

/* Checks a call's error against an op's expectError. */
static void
expecting (SchedulerHarness *h, BroJsonValue *want, gboolean ok, BroBroError *error)
{
  g_autoptr (BroBroError) owned = error;
  if (!want)
    {
      if (!ok)
        bro_test_fail (h->failures, h->id, "unexpected error %s", error->code);
      return;
    }
  if (ok)
    {
      bro_test_fail (h->failures, h->id, "expected an error");
      return;
    }
  if (want->kind == BRO_JSON_VALUE_STRING)
    bro_test_same_string (h->failures, h->id, "error", want->string, error->code);
  else
    {
      bro_test_same_string (h->failures, h->id, "error", bro_json_value_get_string (bro_json_value_member (want, "code"), ""), error->code);
      bro_test_same_json (h->failures, h->id, "error params", bro_json_value_member (want, "params"), error->params);
    }
}

static void
run_scheduler_op (SchedulerHarness *h, BroJsonValue *op)
{
  BroJsonValue *arg;
  const char *name = op_name (op, &arg);
  BroBroError *error = NULL;
  if (strcmp (name, "submit") == 0)
    {
      BroQueue q;
      g_assert_true (bro_queue_from_wire (bro_json_value_get_string (bro_json_value_member (arg, "queue"), ""), &q));
      bro_scheduler_submit (h->scheduler, job_of (h, bro_json_value_get_string (bro_json_value_member (arg, "job"), "")), q);
    }
  else if (strcmp (name, "request") == 0)
    {
      const char *as = bro_json_value_get_string (bro_json_value_member (arg, "as"), "");
      g_autoptr (BroResourceSet) res = bro_resource_set_new ();
      BroCancellationSource *cancel = bro_cancellation_source_new ();
      Asker *a = g_new0 (Asker, 1);
      gboolean ok;
      res->drives = strv_of (bro_json_value_member (arg, "drives"));
      res->acquisition = bro_json_value_get_bool (bro_json_value_member (arg, "acquisition"), FALSE);
      res->library_writes = strv_of (bro_json_value_member (arg, "libraryWrites"));
      res->cpu = bro_json_value_get_integer (bro_json_value_member (arg, "cpu"), 0);
      res->io = strv_of (bro_json_value_member (arg, "io"));
      a->h = h;
      a->name = g_strdup (as);
      g_hash_table_replace (h->cancels, g_strdup (as), cancel);
      g_hash_table_replace (h->waits, g_strdup (as), g_ptr_array_new_with_free_func (g_free));
      ok = bro_scheduler_acquire (h->scheduler, job_of (h, bro_json_value_get_string (bro_json_value_member (arg, "job"), "")), res,
                                  asker_waits, bro_cancellation_source_token (cancel), asker_done, a, asker_free, &error);
      expecting (h, NULL, ok, error);
    }
  else if (strcmp (name, "release") == 0)
    {
      bro_scheduler_release (h->scheduler, g_hash_table_lookup (h->leases, bro_json_value_get_string (arg, "")));
      set_state (h, bro_json_value_get_string (arg, ""), "released");
    }
  else if (strcmp (name, "releaseDrives") == 0)
    bro_scheduler_release_drives (h->scheduler, job_of (h, bro_json_value_get_string (arg, "")));
  else if (strcmp (name, "toProcessing") == 0)
    bro_scheduler_move_to (h->scheduler, job_of (h, bro_json_value_get_string (arg, "")), BRO_QUEUE_PROCESSING);
  else if (strcmp (name, "finish") == 0)
    {
      const BroId *job = job_of (h, bro_json_value_get_string (arg, ""));
      GHashTableIter it;
      gpointer n, lease;
      g_hash_table_iter_init (&it, h->leases);
      while (g_hash_table_iter_next (&it, &n, &lease))
        if (strcmp (((BroLease *) lease)->job_id.value, job->value) == 0 && g_strcmp0 (g_hash_table_lookup (h->states, n), "granted") == 0)
          set_state (h, n, "released");
      bro_scheduler_finish (h->scheduler, job);
    }
  else if (strcmp (name, "move") == 0)
    {
      gboolean ok = bro_scheduler_move (h->scheduler, job_of (h, bro_json_value_get_string (bro_json_value_member (arg, "job"), "")),
                                        bro_json_value_get_integer (bro_json_value_member (arg, "by"), 0), &error);
      expecting (h, bro_json_value_member (arg, "expectError"), ok, error);
    }
  else if (strcmp (name, "startNow") == 0)
    {
      gboolean ok = bro_scheduler_start_now (h->scheduler, job_of (h, bro_json_value_get_string (bro_json_value_member (arg, "job"), "")), &error);
      expecting (h, bro_json_value_member (arg, "expectError"), ok, error);
    }
  else if (strcmp (name, "cancel") == 0)
    bro_cancellation_source_cancel (g_hash_table_lookup (h->cancels, bro_json_value_get_string (arg, "")));
  else if (strcmp (name, "cancelAll") == 0)
    {
      BroQueue q;
      g_assert_true (bro_queue_from_wire (bro_json_value_get_string (arg, ""), &q));
      bro_scheduler_cancel_all (h->scheduler, q);
    }
  else if (strcmp (name, "library") == 0)
    {
      const char *id = bro_json_value_get_string (bro_json_value_member (arg, "id"), "");
      if (bro_json_value_get_bool (bro_json_value_member (arg, "online"), TRUE))
        bro_scheduler_set_library_offline (h->scheduler, id, NULL);
      else
        {
          BroJsonValue *params = bro_json_value_new_object ();
          g_autoptr (BroBroError) reason = NULL;
          bro_json_value_set (params, "library", bro_json_value_new_string (id));
          reason = bro_bro_error_new (bro_message_code_wire (BRO_MSG_LIBRARY_OFFLINE), params, NULL);
          bro_scheduler_set_library_offline (h->scheduler, id, reason);
        }
    }
  else if (strcmp (name, "limits") == 0)
    {
      BroSchedulerLimits l = limits_of (arg);
      bro_scheduler_update_limits (h->scheduler, &l);
    }
  else if (strcmp (name, "reserve") == 0)
    {
      gboolean ok = bro_scheduler_reserve (h->scheduler, job_of (h, bro_json_value_get_string (bro_json_value_member (arg, "job"), "")),
                                           bro_json_value_get_string (bro_json_value_member (arg, "volume"), ""),
                                           bro_json_value_get_string (bro_json_value_member (arg, "path"), ""),
                                           bro_json_value_get_integer (bro_json_value_member (arg, "need"), 0),
                                           bro_json_value_get_integer (bro_json_value_member (arg, "free"), 0), &error);
      expecting (h, bro_json_value_member (arg, "expectError"), ok, error);
    }
  else if (strcmp (name, "unreserve") == 0)
    bro_scheduler_unreserve (h->scheduler, job_of (h, bro_json_value_get_string (arg, "")));
  else
    g_error ("unknown op %s", name);
}

typedef struct {
  SchedulerHarness *h;
  BroJsonValue *op;
} SchedulerOp;

static void
scheduler_op_action (gpointer data)
{
  SchedulerOp *o = data;
  run_scheduler_op (o->h, o->op);
}

static void
scheduler_subscribe_action (gpointer data)
{
  SchedulerHarness *h = data;
  h->subscription = bro_event_bus_subscribe (h->bus, NULL, 0, event_seen, h->events, NULL);
}

static BroJsonValue *
names_json (SchedulerHarness *h, GArray *ids)
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < ids->len; i++)
    bro_json_value_append (a, bro_json_value_new_string (name_of (h, &g_array_index (ids, BroId, i))));
  return a;
}

static int
compare_strings_in_array (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static void
cancel_source_free (gpointer data)
{
  bro_cancellation_source_free (data);
}

static gboolean
scheduler_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  SchedulerHarness h = { 0 };
  BroJsonValue *ops = bro_json_value_member (given, "ops"), *v;
  BroSchedulerLimits limits = limits_of (bro_json_value_member (given, "limits"));
  world_init (&h.w);
  h.bus = bro_event_bus_new (BRO_CLOCK (h.w.clock), 1, 1024, 1024);
  h.scheduler = bro_scheduler_new (h.w.executor, h.bus, &limits);
  h.jobs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  h.granted = g_ptr_array_new_with_free_func (g_free);
  h.states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  h.waits = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  h.leases = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_lease_unref);
  h.cancels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, cancel_source_free);
  h.events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_engine_event_unref);
  h.failures = failures;
  h.id = id;
  bro_engine_executor_post (h.w.executor, scheduler_subscribe_action, &h, NULL);
  for (guint i = 0; i < bro_json_value_length (ops); i++)
    {
      SchedulerOp o = { &h, bro_json_value_at (ops, i) };
      bro_engine_executor_post (h.w.executor, scheduler_op_action, &o, NULL);
      bro_engine_executor_barrier (h.w.executor);
      bro_engine_executor_barrier (h.w.executor); /* and the answers it posted */
    }
  if ((v = bro_json_value_member (expect, "granted")))
    {
      /* "<seq> <name>", sorted: grant order, then the names alone */
      g_autoptr (GPtrArray) names = g_ptr_array_new ();
      g_ptr_array_sort (h.granted, compare_strings_in_array);
      for (guint i = 0; i < h.granted->len; i++)
        g_ptr_array_add (names, strchr (g_ptr_array_index (h.granted, i), ' ') + 1);
      same_lines (failures, id, "granted", v, names);
    }
  if ((v = bro_json_value_member (expect, "states")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        const char *name = g_ptr_array_index (v->keys, i);
        g_autofree char *what = g_strdup_printf ("state of %s", name);
        bro_test_same_string (failures, id, what, bro_json_value_get_string (g_ptr_array_index (v->items, i), ""),
                              g_hash_table_lookup (h.states, name));
      }
  if ((v = bro_json_value_member (expect, "waits")))
    for (guint i = 0; i < v->keys->len; i++)
      {
        const char *name = g_ptr_array_index (v->keys, i);
        g_autofree char *what = g_strdup_printf ("waits of %s", name);
        same_lines (failures, id, what, g_ptr_array_index (v->items, i), g_hash_table_lookup (h.waits, name));
      }
  if ((v = bro_json_value_member (expect, "queues")))
    {
      g_autoptr (BroQueueOrders) q = bro_scheduler_queues (h.scheduler);
      g_autoptr (BroJsonValue) got = bro_json_value_new_object ();
      bro_json_value_set (got, "acquisition", names_json (&h, q->acquisition));
      bro_json_value_set (got, "processing", names_json (&h, q->processing));
      bro_json_value_set (got, "maintenance", names_json (&h, q->maintenance));
      bro_test_same_json (failures, id, "queues", v, got);
    }
  if ((v = bro_json_value_member (expect, "leases")))
    {
      g_autoptr (BroLeaseSnapshot) s = bro_scheduler_leases (h.scheduler);
      g_autoptr (BroJsonValue) got = bro_lease_snapshot_json (s);
      bro_test_same_json (failures, id, "leases", v, got);
    }
  if ((v = bro_json_value_member (expect, "events")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      for (guint i = 0; i < h.events->len; i++)
        bro_json_value_append (got, bro_engine_event_json (g_ptr_array_index (h.events, i)));
      bro_test_same_json (failures, id, "events", v, got);
    }
  bro_engine_executor_stop (h.w.executor);
  g_clear_object (&h.subscription);
  g_object_unref (h.scheduler);
  g_object_unref (h.bus);
  g_hash_table_unref (h.leases);
  g_hash_table_unref (h.cancels);
  g_hash_table_unref (h.jobs);
  g_ptr_array_unref (h.granted);
  g_hash_table_unref (h.states);
  g_hash_table_unref (h.waits);
  g_ptr_array_unref (h.events);
  world_clear (&h.w);
  return TRUE;
}

static void
test_scheduler (void)
{
  bro_test_run_cases ("engine/scheduler.cases.json", scheduler_case);
}

static void
test_lease_snapshot (void)
{
  g_autoptr (BroLeaseSnapshot) s = bro_lease_snapshot_new ();
  g_autoptr (BroLeaseSnapshot) back = NULL;
  g_autoptr (BroJsonValue) json = NULL, again = NULL, bad = bro_json_value_parse ("{\"drives\": 1}", -1);
  BroId job = { "00000000-0000-4000-8000-000000000001" };
  g_ptr_array_add (s->drives, bro_drive_holder_new ("drv-0123456789abcdef", &job));
  s->acquisitions = 1;
  s->jobs = (BroJobCounts) { 1, 2, 3 };
  json = bro_lease_snapshot_json (s);
  back = bro_lease_snapshot_parse (json);
  g_assert_nonnull (back);
  again = bro_lease_snapshot_json (back);
  g_assert_true (bro_json_value_equal (json, again));
  g_assert_null (bro_lease_snapshot_parse (bad));
}
