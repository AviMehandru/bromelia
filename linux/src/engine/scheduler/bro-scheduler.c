/* bro-scheduler.c */
#include "bro-scheduler.h"

#include "bro-message-code.h"

#include <string.h>

#define ACQUISITION_KEY "acquisition"
#define PROCESSING_KEY "processing"
#define CPU_KEY "cpu"

typedef struct {
  BroId job;
  BroResourceSet *resources; /* a copy */
  BroWaitFunc on_wait;
  BroAcquireDone done;
  gpointer data;
  GDestroyNotify destroy;
  guint64 order;
  gboolean has_last;
  BroWaitKind last_kind;
  BroBroError *last_reason;
  BroCancellationToken *cancel; /* a reference, while connected */
  guint cancel_id;
  BroLease *lease; /* the outcome */
  BroBroError *error;
} Request;

typedef struct {
  BroId job;
  char *volume;
  gint64 bytes;
} Reservation;

typedef struct {
  const char *key;
  int count;
} Need;

struct _BroScheduler {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroSchedulerLimits limits;
  GArray *queues[3];          /* of BroId, by BroQueue */
  GHashTable *queue_of;       /* job id → GINT_TO_POINTER (queue + 1) */
  GHashTable *start_now;      /* job ids */
  GPtrArray *pending;         /* of Request, owned */
  GHashTable *drive_holder;   /* drive → job id */
  GHashTable *held;           /* key → GINT_TO_POINTER (count) */
  GHashTable *leases;         /* job id → GPtrArray of BroLease */
  GHashTable *offline;        /* library id → BroBroError */
  GPtrArray *reserved;        /* of Reservation */
  BroJsonValue *published;    /* the last leaseChanged data */
  guint64 next_order;
  gint64 granted;
};

G_DEFINE_FINAL_TYPE (BroScheduler, bro_scheduler, G_TYPE_OBJECT)

static void pump (BroScheduler *self);

/* ---- small helpers ---- */

static int
held_count (GHashTable *table, const char *key)
{
  return GPOINTER_TO_INT (g_hash_table_lookup (table, key));
}

static void
add_count (GHashTable *table, const char *key, int n)
{
  g_hash_table_replace (table, g_strdup (key), GINT_TO_POINTER (held_count (table, key) + n));
}

static gboolean
in_queue (BroScheduler *self, const BroId *job, BroQueue *queue)
{
  gpointer v = g_hash_table_lookup (self->queue_of, job->value);
  if (!v)
    return FALSE;
  if (queue)
    *queue = GPOINTER_TO_INT (v) - 1;
  return TRUE;
}

static int
index_in (GArray *order, const BroId *job)
{
  for (guint i = 0; i < order->len; i++)
    if (strcmp (g_array_index (order, BroId, i).value, job->value) == 0)
      return i;
  return -1;
}

static BroResourceSet *
copy_resources (const BroResourceSet *r)
{
  BroResourceSet *c = bro_resource_set_new ();
  c->drives = g_strdupv (r->drives);
  c->acquisition = r->acquisition;
  c->library_writes = g_strdupv (r->library_writes);
  c->cpu = r->cpu;
  c->io = g_strdupv (r->io);
  return c;
}

static void
request_free (gpointer data)
{
  Request *r = data;
  if (r->cancel)
    {
      bro_cancellation_token_disconnect (r->cancel, r->cancel_id);
      bro_cancellation_token_unref (r->cancel);
    }
  if (r->destroy)
    r->destroy (r->data);
  bro_resource_set_free (r->resources);
  bro_bro_error_free (r->last_reason);
  bro_bro_error_free (r->error);
  bro_lease_unref (r->lease);
  g_free (r);
}

static void
deliver (gpointer data)
{
  Request *r = data;
  r->done (r->lease ? bro_lease_ref (r->lease) : NULL, r->error, r->data);
}

/* The request ends with @lease (taken) or @error (taken): @done is posted. */
static void
resolve (BroScheduler *self, Request *r, BroLease *lease, BroBroError *error)
{
  guint at;
  if (g_ptr_array_find (self->pending, r, &at))
    g_ptr_array_steal_index (self->pending, at);
  if (r->cancel)
    {
      bro_cancellation_token_disconnect (r->cancel, r->cancel_id);
      g_clear_pointer (&r->cancel, bro_cancellation_token_unref);
    }
  r->lease = lease;
  r->error = error;
  bro_engine_executor_post (self->executor, deliver, r, request_free);
}

static void
cancel_request (BroScheduler *self, Request *r)
{
  resolve (self, r, NULL, bro_bro_error_new (bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL, NULL));
}

static void
publish_queue (BroScheduler *self, BroQueue queue)
{
  g_autoptr (BroQueueChange) change = bro_queue_change_new (queue, self->queues[queue]);
  g_autoptr (BroJsonValue) data = bro_queue_change_json (change);
  bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_QUEUE_CHANGED, data));
}

static gboolean
not_queued (const BroId *job, BroBroError **error)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "jobId", bro_json_value_new_string (job->value));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_NOT_QUEUED), params);
  return FALSE;
}

/* ---- queues ---- */

void
bro_scheduler_submit (BroScheduler *self, const BroId *job_id, BroQueue queue)
{
  if (in_queue (self, job_id, NULL))
    return;
  g_hash_table_insert (self->queue_of, g_strdup (job_id->value), GINT_TO_POINTER (queue + 1));
  g_array_append_val (self->queues[queue], *job_id);
  publish_queue (self, queue);
  pump (self);
}

void
bro_scheduler_move_to (BroScheduler *self, const BroId *job_id, BroQueue queue)
{
  BroQueue from;
  if (!in_queue (self, job_id, &from) || from == queue)
    return;
  g_array_remove_index (self->queues[from], index_in (self->queues[from], job_id));
  g_hash_table_replace (self->queue_of, g_strdup (job_id->value), GINT_TO_POINTER (queue + 1));
  g_array_append_val (self->queues[queue], *job_id);
  publish_queue (self, from);
  publish_queue (self, queue);
  pump (self);
}

gboolean
bro_scheduler_move (BroScheduler *self, const BroId *job_id, int by, BroBroError **error)
{
  BroQueue queue;
  GArray *order;
  int from, to;
  if (!in_queue (self, job_id, &queue))
    return not_queued (job_id, error);
  order = self->queues[queue];
  from = index_in (order, job_id);
  to = CLAMP (from + by, 0, (int) order->len - 1);
  if (from != to)
    {
      BroId id = *job_id;
      g_array_remove_index (order, from);
      g_array_insert_val (order, to, id);
      publish_queue (self, queue);
      pump (self);
    }
  return TRUE;
}

gboolean
bro_scheduler_start_now (BroScheduler *self, const BroId *job_id, BroBroError **error)
{
  BroQueue queue;
  GArray *order;
  int at;
  if (!in_queue (self, job_id, &queue))
    return not_queued (job_id, error);
  g_hash_table_add (self->start_now, g_strdup (job_id->value));
  order = self->queues[queue];
  at = index_in (order, job_id);
  if (at != 0)
    {
      BroId id = *job_id;
      g_array_remove_index (order, at);
      g_array_prepend_val (order, id);
      publish_queue (self, queue);
    }
  pump (self);
  return TRUE;
}

static void
give_back (BroScheduler *self, BroLease *lease)
{
  lease->released = TRUE;
  for (guint i = 0; i < lease->counted_keys->len; i++)
    add_count (self->held, g_ptr_array_index (lease->counted_keys, i), -g_array_index (lease->counted_counts, int, i));
}

static void
drop_drives (BroScheduler *self, const BroId *job)
{
  GHashTableIter it;
  gpointer holder;
  g_hash_table_iter_init (&it, self->drive_holder);
  while (g_hash_table_iter_next (&it, NULL, &holder))
    if (strcmp (holder, job->value) == 0)
      g_hash_table_iter_remove (&it);
}

void
bro_scheduler_finish (BroScheduler *self, const BroId *job_id)
{
  GPtrArray *leases;
  BroQueue queue;
  for (guint i = self->pending->len; i > 0; i--)
    {
      Request *r = g_ptr_array_index (self->pending, i - 1);
      if (strcmp (r->job.value, job_id->value) == 0)
        cancel_request (self, r);
    }
  if ((leases = g_hash_table_lookup (self->leases, job_id->value)))
    {
      for (guint i = 0; i < leases->len; i++)
        give_back (self, g_ptr_array_index (leases, i));
      g_hash_table_remove (self->leases, job_id->value);
    }
  drop_drives (self, job_id);
  bro_scheduler_unreserve (self, job_id);
  g_hash_table_remove (self->start_now, job_id->value);
  if (in_queue (self, job_id, &queue))
    {
      g_array_remove_index (self->queues[queue], index_in (self->queues[queue], job_id));
      g_hash_table_remove (self->queue_of, job_id->value);
      publish_queue (self, queue);
    }
  pump (self);
}

int
bro_scheduler_position (BroScheduler *self, const BroId *job_id)
{
  gpointer q = g_hash_table_lookup (self->queue_of, job_id->value);
  return q ? index_in (self->queues[GPOINTER_TO_INT (q) - 1], job_id) : -1;
}

BroQueueOrders *
bro_scheduler_queues (BroScheduler *self)
{
  BroQueueOrders *o = bro_queue_orders_new ();
  g_array_append_vals (o->acquisition, self->queues[BRO_QUEUE_ACQUISITION]->data, self->queues[BRO_QUEUE_ACQUISITION]->len);
  g_array_append_vals (o->processing, self->queues[BRO_QUEUE_PROCESSING]->data, self->queues[BRO_QUEUE_PROCESSING]->len);
  g_array_append_vals (o->maintenance, self->queues[BRO_QUEUE_MAINTENANCE]->data, self->queues[BRO_QUEUE_MAINTENANCE]->len);
  return o;
}

/* ---- leases ---- */

typedef struct {
  GWeakRef scheduler;
  guint64 order;
} CancelProbe;

static void
cancel_probe_free (gpointer data)
{
  CancelProbe *p = data;
  g_weak_ref_clear (&p->scheduler);
  g_free (p);
}

static void
cancel_on_executor (gpointer data)
{
  CancelProbe *p = data;
  g_autoptr (BroScheduler) self = g_weak_ref_get (&p->scheduler);
  if (!self)
    return;
  for (guint i = 0; i < self->pending->len; i++)
    {
      Request *r = g_ptr_array_index (self->pending, i);
      if (r->order == p->order)
        {
          cancel_request (self, r);
          pump (self);
          return;
        }
    }
}

/* On whatever thread cancelled: the request is found again by its number on the executor (it may be gone). */
static void
cancelled (gpointer data, gpointer unused)
{
  CancelProbe *p = data;
  g_autoptr (BroScheduler) self = g_weak_ref_get (&p->scheduler);
  CancelProbe *copy;
  if (!self)
    return;
  copy = g_new0 (CancelProbe, 1);
  g_weak_ref_init (&copy->scheduler, self);
  copy->order = p->order;
  bro_engine_executor_post (self->executor, cancel_on_executor, copy, cancel_probe_free);
}

gboolean
bro_scheduler_acquire (BroScheduler *self, const BroId *job_id, const BroResourceSet *resources, BroWaitFunc on_wait,
                       BroCancellationToken *cancel, BroAcquireDone done, gpointer data, GDestroyNotify destroy,
                       BroBroError **error)
{
  Request *r;
  if (!in_queue (self, job_id, NULL))
    {
      if (destroy)
        destroy (data);
      return not_queued (job_id, error);
    }
  r = g_new0 (Request, 1);
  r->job = *job_id;
  r->resources = copy_resources (resources);
  r->on_wait = on_wait;
  r->done = done;
  r->data = data;
  r->destroy = destroy;
  r->order = self->next_order++;
  g_ptr_array_add (self->pending, r);
  if (cancel)
    {
      CancelProbe *p = g_new0 (CancelProbe, 1);
      g_weak_ref_init (&p->scheduler, self);
      p->order = r->order;
      r->cancel = bro_cancellation_token_ref (cancel);
      r->cancel_id = bro_cancellation_token_on_cancel (cancel, cancelled, p, cancel_probe_free);
    }
  pump (self);
  return TRUE;
}

void
bro_scheduler_cancel_all (BroScheduler *self, BroQueue queue)
{
  for (guint i = self->pending->len; i > 0; i--)
    {
      Request *r = g_ptr_array_index (self->pending, i - 1);
      BroQueue q;
      if (in_queue (self, &r->job, &q) && q == queue)
        cancel_request (self, r);
    }
  pump (self);
}

void
bro_scheduler_release (BroScheduler *self, BroLease *lease)
{
  GPtrArray *leases;
  if (lease->released)
    return;
  give_back (self, lease);
  if ((leases = g_hash_table_lookup (self->leases, lease->job_id.value)))
    {
      g_ptr_array_remove (leases, lease);
      if (leases->len == 0)
        g_hash_table_remove (self->leases, lease->job_id.value);
    }
  pump (self);
}

void
bro_scheduler_release_drives (BroScheduler *self, const BroId *job_id)
{
  drop_drives (self, job_id);
  pump (self);
}

void
bro_scheduler_set_library_offline (BroScheduler *self, const char *library_id, const BroBroError *reason)
{
  if (reason)
    g_hash_table_replace (self->offline, g_strdup (library_id), bro_bro_error_copy (reason));
  else
    g_hash_table_remove (self->offline, library_id);
  pump (self);
}

void
bro_scheduler_update_limits (BroScheduler *self, const BroSchedulerLimits *limits)
{
  self->limits = *limits;
  pump (self);
}

static int
compare_strings (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

BroLeaseSnapshot *
bro_scheduler_leases (BroScheduler *self)
{
  BroLeaseSnapshot *s = bro_lease_snapshot_new ();
  g_autoptr (GPtrArray) drives = g_ptr_array_new ();
  g_autoptr (GHashTable) holding = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, self->drive_holder);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      g_ptr_array_add (drives, key);
      g_hash_table_add (holding, value);
    }
  g_ptr_array_sort (drives, compare_strings);
  for (guint i = 0; i < drives->len; i++)
    {
      BroId job = { { 0 } };
      g_strlcpy (job.value, g_hash_table_lookup (self->drive_holder, g_ptr_array_index (drives, i)), sizeof job.value);
      g_ptr_array_add (s->drives, bro_drive_holder_new (g_ptr_array_index (drives, i), &job));
    }
  g_hash_table_iter_init (&it, self->leases);
  while (g_hash_table_iter_next (&it, &key, NULL))
    g_hash_table_add (holding, key);
  g_hash_table_iter_init (&it, holding);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      gpointer q = g_hash_table_lookup (self->queue_of, key);
      if (q && GPOINTER_TO_INT (q) - 1 == BRO_QUEUE_ACQUISITION)
        s->jobs.acquisition++;
      else if (q && GPOINTER_TO_INT (q) - 1 == BRO_QUEUE_PROCESSING)
        s->jobs.processing++;
      else if (q)
        s->jobs.maintenance++;
    }
  s->acquisitions = held_count (self->held, ACQUISITION_KEY);
  return s;
}

/* ---- space ---- */

gboolean
bro_scheduler_reserve (BroScheduler *self, const BroId *job_id, const char *volume, const char *path, gint64 need,
                       gint64 free, BroBroError **error)
{
  gint64 others = 0, available;
  Reservation *mine = NULL;
  for (guint i = 0; i < self->reserved->len; i++)
    {
      Reservation *r = g_ptr_array_index (self->reserved, i);
      if (strcmp (r->volume, volume) != 0)
        continue;
      if (strcmp (r->job.value, job_id->value) == 0)
        mine = r;
      else
        others += r->bytes;
    }
  available = MAX (0, free - others);
  if (available < need)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "path", bro_json_value_new_string (path));
      bro_json_value_set (params, "need", bro_json_value_new_integer (need));
      bro_json_value_set (params, "free", bro_json_value_new_integer (available));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_SPACE_INSUFFICIENT), params);
      return FALSE;
    }
  if (!mine)
    {
      mine = g_new0 (Reservation, 1);
      mine->job = *job_id;
      mine->volume = g_strdup (volume);
      g_ptr_array_add (self->reserved, mine);
    }
  mine->bytes = need;
  return TRUE;
}

static void
reservation_free (gpointer data)
{
  Reservation *r = data;
  g_free (r->volume);
  g_free (r);
}

void
bro_scheduler_unreserve (BroScheduler *self, const BroId *job_id)
{
  for (guint i = self->reserved->len; i > 0; i--)
    if (strcmp (((Reservation *) g_ptr_array_index (self->reserved, i - 1))->job.value, job_id->value) == 0)
      g_ptr_array_remove_index (self->reserved, i - 1);
}

/* ---- granting ---- */

static int
rank (BroQueue q)
{
  return q == BRO_QUEUE_ACQUISITION ? 0 : q == BRO_QUEUE_PROCESSING ? 1 : 2;
}

static int
capacity (BroScheduler *self, const char *key, const BroId *job)
{
  if (strcmp (key, ACQUISITION_KEY) == 0)
    return self->limits.acquisitions == 0 || g_hash_table_contains (self->start_now, job->value) ? G_MAXINT
                                                                                                 : self->limits.acquisitions;
  if (strcmp (key, PROCESSING_KEY) == 0)
    return MAX (1, self->limits.processing);
  if (strcmp (key, CPU_KEY) == 0)
    return MAX (1, self->limits.cpu);
  return MAX (1, self->limits.io_per_volume); /* io:<volume> */
}

/* The counted resources of @r: keys point into @keys (owned by the caller). */
static GArray *
counted (BroScheduler *self, Request *r, GPtrArray *keys)
{
  GArray *needs = g_array_new (FALSE, FALSE, sizeof (Need));
  BroQueue queue = BRO_QUEUE_ACQUISITION;
  Need n;
  in_queue (self, &r->job, &queue);
  if (r->resources->acquisition)
    {
      n = (Need) { ACQUISITION_KEY, 1 };
      g_array_append_val (needs, n);
    }
  if (queue == BRO_QUEUE_PROCESSING)
    {
      n = (Need) { PROCESSING_KEY, 1 };
      g_array_append_val (needs, n);
    }
  if (r->resources->cpu > 0)
    {
      n = (Need) { CPU_KEY, MIN (r->resources->cpu, capacity (self, CPU_KEY, &r->job)) };
      g_array_append_val (needs, n);
    }
  for (guint i = 0; r->resources->io && r->resources->io[i]; i++)
    {
      gboolean seen = FALSE;
      for (guint k = 0; k < i; k++)
        seen |= strcmp (r->resources->io[k], r->resources->io[i]) == 0;
      if (seen)
        continue;
      g_ptr_array_add (keys, g_strconcat ("io:", r->resources->io[i], NULL));
      n = (Need) { g_ptr_array_index (keys, keys->len - 1), 1 };
      g_array_append_val (needs, n);
    }
  return needs;
}

static int
compare_requests (gconstpointer a, gconstpointer b, gpointer data)
{
  BroScheduler *self = data;
  const Request *ra = *(Request *const *) a, *rb = *(Request *const *) b;
  BroQueue qa = BRO_QUEUE_MAINTENANCE, qb = BRO_QUEUE_MAINTENANCE;
  int ia, ib;
  in_queue (self, &ra->job, &qa);
  in_queue (self, &rb->job, &qb);
  if (rank (qa) != rank (qb))
    return rank (qa) - rank (qb);
  ia = index_in (self->queues[qa], &ra->job);
  ib = index_in (self->queues[qb], &rb->job);
  if (ia != ib)
    return ia - ib;
  return ra->order < rb->order ? -1 : ra->order > rb->order;
}

static void
grant (BroScheduler *self, Request *r, GArray *needs)
{
  BroLease *lease = bro_lease_new (&r->job, ++self->granted);
  GPtrArray *list = g_hash_table_lookup (self->leases, r->job.value);
  for (guint i = 0; r->resources->drives && r->resources->drives[i]; i++)
    g_hash_table_replace (self->drive_holder, g_strdup (r->resources->drives[i]), g_strdup (r->job.value));
  for (guint i = 0; i < needs->len; i++)
    {
      Need *n = &g_array_index (needs, Need, i);
      add_count (self->held, n->key, n->count);
      g_ptr_array_add (lease->counted_keys, g_strdup (n->key));
      g_array_append_val (lease->counted_counts, n->count);
    }
  if (!list)
    {
      list = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_lease_unref);
      g_hash_table_insert (self->leases, g_strdup (r->job.value), list);
    }
  g_ptr_array_add (list, bro_lease_ref (lease));
  resolve (self, r, lease, NULL);
}

static gboolean
same_wait (Request *r, BroWaitKind kind, const BroBroError *reason)
{
  if (!r->has_last || r->last_kind != kind)
    return FALSE;
  if (!reason || !r->last_reason)
    return reason == r->last_reason;
  return strcmp (reason->code, r->last_reason->code) == 0 && bro_json_value_equal (reason->params, r->last_reason->params);
}

static void
pump (BroScheduler *self)
{
  g_autoptr (GHashTable) claimed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) waited_drives = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) order = g_ptr_array_new (); /* not g_ptr_array_copy: that keeps the free function */
  g_autoptr (BroLeaseSnapshot) snapshot = NULL;
  g_autoptr (BroJsonValue) json = NULL;
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init (&it, self->held);
  while (g_hash_table_iter_next (&it, &key, &value))
    g_hash_table_insert (claimed, g_strdup (key), value);
  g_ptr_array_extend (order, self->pending, NULL, NULL);
  g_ptr_array_sort_with_data (order, compare_requests, self);
  for (guint i = 0; i < order->len; i++)
    {
      Request *r = g_ptr_array_index (order, i);
      g_autoptr (GPtrArray) keys = g_ptr_array_new_with_free_func (g_free);
      g_autoptr (GArray) needs = counted (self, r, keys);
      g_autoptr (GPtrArray) wait_drives = g_ptr_array_new ();
      g_autoptr (GArray) wait_keys = g_array_new (FALSE, FALSE, sizeof (Need));
      BroQueue queue = BRO_QUEUE_ACQUISITION;
      const BroBroError *blocked = NULL;
      gboolean gated;
      in_queue (self, &r->job, &queue);
      for (guint d = 0; r->resources->drives && r->resources->drives[d]; d++)
        {
          const char *drive = r->resources->drives[d];
          const char *holder = g_hash_table_lookup (self->drive_holder, drive);
          gboolean mine = holder && strcmp (holder, r->job.value) == 0;
          if ((holder && !mine) || (g_hash_table_contains (waited_drives, drive) && !mine))
            g_ptr_array_add (wait_drives, (gpointer) drive);
        }
      for (guint n = 0; n < needs->len; n++)
        {
          Need *need = &g_array_index (needs, Need, n);
          if (held_count (claimed, need->key) + need->count > capacity (self, need->key, &r->job))
            g_array_append_val (wait_keys, *need);
        }
      gated = queue == BRO_QUEUE_MAINTENANCE && held_count (self->held, ACQUISITION_KEY) > 0;
      for (guint l = 0; !blocked && r->resources->library_writes && r->resources->library_writes[l]; l++)
        blocked = g_hash_table_lookup (self->offline, r->resources->library_writes[l]);
      if (!blocked && !gated && wait_drives->len == 0 && wait_keys->len == 0)
        {
          for (guint n = 0; n < needs->len; n++)
            add_count (claimed, g_array_index (needs, Need, n).key, g_array_index (needs, Need, n).count);
          grant (self, r, needs);
          continue;
        }
      for (guint d = 0; d < wait_drives->len; d++)
        g_hash_table_add (waited_drives, g_ptr_array_index (wait_drives, d));
      for (guint n = 0; n < wait_keys->len; n++)
        add_count (claimed, g_array_index (wait_keys, Need, n).key, g_array_index (wait_keys, Need, n).count);
      if (!same_wait (r, blocked ? BRO_WAIT_BLOCKED : BRO_WAIT_WAITING, blocked))
        {
          g_autoptr (BroWait) wait = bro_wait_new (blocked ? BRO_WAIT_BLOCKED : BRO_WAIT_WAITING, blocked);
          r->has_last = TRUE;
          r->last_kind = wait->kind;
          g_clear_pointer (&r->last_reason, bro_bro_error_free);
          r->last_reason = blocked ? bro_bro_error_copy (blocked) : NULL;
          if (r->on_wait)
            r->on_wait (wait, r->data);
        }
    }
  snapshot = bro_scheduler_leases (self);
  json = bro_lease_snapshot_json (snapshot);
  if (!bro_json_value_equal (json, self->published))
    {
      g_clear_pointer (&self->published, bro_json_value_unref);
      self->published = bro_json_value_ref (json);
      bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_LEASE_CHANGED, json));
    }
}

/* ---- the object ---- */

BroScheduler *
bro_scheduler_new (BroEngineExecutor *executor, BroEventBus *bus, const BroSchedulerLimits *limits)
{
  BroScheduler *self = g_object_new (BRO_TYPE_SCHEDULER, NULL);
  g_autoptr (BroLeaseSnapshot) empty = bro_lease_snapshot_new ();
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->limits = *limits;
  self->published = bro_lease_snapshot_json (empty);
  return self;
}

static void
bro_scheduler_finalize (GObject *object)
{
  BroScheduler *self = BRO_SCHEDULER (object);
  g_ptr_array_unref (self->pending); /* requests never answered: their data is destroyed, done doesn't run */
  for (int q = 0; q < 3; q++)
    g_array_unref (self->queues[q]);
  g_hash_table_unref (self->queue_of);
  g_hash_table_unref (self->start_now);
  g_hash_table_unref (self->drive_holder);
  g_hash_table_unref (self->held);
  g_hash_table_unref (self->leases);
  g_hash_table_unref (self->offline);
  g_ptr_array_unref (self->reserved);
  bro_json_value_unref (self->published);
  g_object_unref (self->bus);
  g_object_unref (self->executor);
  G_OBJECT_CLASS (bro_scheduler_parent_class)->finalize (object);
}

static void
bro_scheduler_class_init (BroSchedulerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_scheduler_finalize;
}

static void
bro_scheduler_init (BroScheduler *self)
{
  for (int q = 0; q < 3; q++)
    self->queues[q] = g_array_new (FALSE, FALSE, sizeof (BroId));
  self->queue_of = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->start_now = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->pending = g_ptr_array_new_with_free_func (request_free);
  self->drive_holder = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->held = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->leases = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  self->offline = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_bro_error_free);
  self->reserved = g_ptr_array_new_with_free_func (reservation_free);
}
