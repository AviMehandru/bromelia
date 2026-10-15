/* test-engine.c: the Engine layer's core, against the case files in shared/fixtures/engine. */
#include "bro-test-fixtures.h"

#include "bro-coalescer.h"
#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"

#include <stdio.h>
#include <string.h>

#define START_MS G_GINT64_CONSTANT (1790971200000) /* 2026-10-02T20:00:00.000Z */

/* ---- a clock that moves only when told (the scenarios' fake clock) ------------------------------------------- */

typedef struct {
  gatomicrefcount refs;
  gint64 due_ms;
  gint64 interval_ms;
  guint64 order;
  gboolean cancelled; /* under the clock's lock */
  BroTimerFunc handler;
  gpointer data;
  GDestroyNotify destroy;
} Pending;

static Pending *
pending_ref (Pending *p)
{
  g_atomic_ref_count_inc (&p->refs);
  return p;
}

static void
pending_unref (Pending *p)
{
  if (!g_atomic_ref_count_dec (&p->refs))
    return;
  if (p->destroy)
    p->destroy (p->data);
  g_free (p);
}

#define TEST_TYPE_MANUAL_CLOCK (test_manual_clock_get_type ())
G_DECLARE_FINAL_TYPE (TestManualClock, test_manual_clock, TEST, MANUAL_CLOCK, GObject)

struct _TestManualClock {
  GObject parent_instance;
  GMutex lock;
  gint64 now_ms;
  guint64 made;
  GPtrArray *timers; /* of Pending, references */
};

#define TEST_TYPE_MANUAL_TIMER (test_manual_timer_get_type ())
G_DECLARE_FINAL_TYPE (TestManualTimer, test_manual_timer, TEST, MANUAL_TIMER, GObject)

struct _TestManualTimer {
  GObject parent_instance;
  TestManualClock *clock; /* a reference */
  Pending *pending;       /* a reference */
};

static void test_manual_timer_iface_init (BroTimerHandleInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestManualTimer, test_manual_timer, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_TIMER_HANDLE, test_manual_timer_iface_init))

static void
manual_timer_cancel (BroTimerHandle *handle)
{
  TestManualTimer *t = TEST_MANUAL_TIMER (handle);
  g_mutex_lock (&t->clock->lock);
  t->pending->cancelled = TRUE;
  g_mutex_unlock (&t->clock->lock);
}

static void
test_manual_timer_iface_init (BroTimerHandleInterface *iface)
{
  iface->cancel = manual_timer_cancel;
}

static void
test_manual_timer_finalize (GObject *object)
{
  TestManualTimer *t = TEST_MANUAL_TIMER (object);
  pending_unref (t->pending);
  g_object_unref (t->clock);
  G_OBJECT_CLASS (test_manual_timer_parent_class)->finalize (object);
}

static void
test_manual_timer_class_init (TestManualTimerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_manual_timer_finalize;
}

static void
test_manual_timer_init (TestManualTimer *self)
{
}

static void test_manual_clock_iface_init (BroClockInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestManualClock, test_manual_clock, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, test_manual_clock_iface_init))

static BroInstant
manual_now (BroClock *clock)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (clock);
  BroInstant at;
  g_mutex_lock (&self->lock);
  at.unix_milliseconds = self->now_ms;
  g_mutex_unlock (&self->lock);
  return at;
}

static BroDuration
manual_monotonic (BroClock *clock)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (clock);
  BroDuration d;
  g_mutex_lock (&self->lock);
  d.seconds = (double) (self->now_ms - START_MS) / 1000;
  g_mutex_unlock (&self->lock);
  return d;
}

static gboolean
manual_sleep (BroClock *clock, BroDuration duration, BroCancellationToken *cancel, BroBroError **error)
{
  g_assert_not_reached ();
}

static BroTimerHandle *
manual_timer (BroClock *clock, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (clock);
  TestManualTimer *t = g_object_new (TEST_TYPE_MANUAL_TIMER, NULL);
  Pending *p = g_new0 (Pending, 1);
  g_atomic_ref_count_init (&p->refs);
  p->handler = handler;
  p->data = data;
  p->destroy = destroy;
  g_mutex_lock (&self->lock);
  p->order = self->made++;
  if (schedule->kind == BRO_TIMER_SCHEDULE_AT)
    p->due_ms = schedule->instant.unix_milliseconds;
  else
    {
      p->interval_ms = MAX (1, (gint64) (schedule->interval.seconds * 1000 + 0.5));
      p->due_ms = self->now_ms + p->interval_ms;
    }
  g_ptr_array_add (self->timers, pending_ref (p));
  g_mutex_unlock (&self->lock);
  t->clock = g_object_ref (self);
  t->pending = p;
  return BRO_TIMER_HANDLE (t);
}

static void
test_manual_clock_iface_init (BroClockInterface *iface)
{
  iface->now = manual_now;
  iface->monotonic = manual_monotonic;
  iface->sleep = manual_sleep;
  iface->timer = manual_timer;
}

static void
test_manual_clock_finalize (GObject *object)
{
  TestManualClock *self = TEST_MANUAL_CLOCK (object);
  g_ptr_array_unref (self->timers);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (test_manual_clock_parent_class)->finalize (object);
}

static void
test_manual_clock_class_init (TestManualClockClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_manual_clock_finalize;
}

static void
test_manual_clock_init (TestManualClock *self)
{
  g_mutex_init (&self->lock);
  self->now_ms = START_MS;
  self->timers = g_ptr_array_new_with_free_func ((GDestroyNotify) pending_unref);
}

/* Fires each due timer in due order (ties in the order they were made), with the time set to its due time, then
 * calls @settle. */
static void
manual_advance (TestManualClock *self, gint64 ms, void (*settle) (gpointer), gpointer settle_data)
{
  gint64 target;
  g_mutex_lock (&self->lock);
  target = self->now_ms + ms;
  g_mutex_unlock (&self->lock);
  for (;;)
    {
      Pending *next = NULL;
      g_mutex_lock (&self->lock);
      for (guint i = self->timers->len; i > 0; i--)
        if (((Pending *) g_ptr_array_index (self->timers, i - 1))->cancelled)
          g_ptr_array_remove_index (self->timers, i - 1);
      for (guint i = 0; i < self->timers->len; i++)
        {
          Pending *p = g_ptr_array_index (self->timers, i);
          if (p->due_ms <= target && (!next || p->due_ms < next->due_ms || (p->due_ms == next->due_ms && p->order < next->order)))
            next = p;
        }
      if (!next)
        {
          self->now_ms = target;
          g_mutex_unlock (&self->lock);
          return;
        }
      pending_ref (next);
      self->now_ms = MAX (self->now_ms, next->due_ms);
      if (next->interval_ms > 0)
        next->due_ms += next->interval_ms;
      else
        g_ptr_array_remove (self->timers, next);
      g_mutex_unlock (&self->lock);
      next->handler (next->data);
      pending_unref (next);
      if (settle)
        settle (settle_data);
    }
}

/* ---- helpers ------------------------------------------------------------------------------------------------- */

static GArray *
types_of (BroJsonValue *j)
{
  GArray *types = g_array_new (FALSE, FALSE, sizeof (BroEngineEventType));
  for (guint i = 0; j && i < bro_json_value_length (j); i++)
    {
      BroEngineEventType t;
      g_assert_true (bro_engine_event_type_from_wire (bro_json_value_get_string (bro_json_value_at (j, i), ""), &t));
      g_array_append_val (types, t);
    }
  return types;
}

static char *
joined (GPtrArray *lines)
{
  GString *s = g_string_new ("");
  for (guint i = 0; lines && i < lines->len; i++)
    g_string_append_printf (s, "%s%s", i ? " | " : "", (char *) g_ptr_array_index (lines, i));
  return g_string_free (s, FALSE);
}

static char *
joined_json (BroJsonValue *list)
{
  GString *s = g_string_new ("");
  for (guint i = 0; i < bro_json_value_length (list); i++)
    g_string_append_printf (s, "%s%s", i ? " | " : "", bro_json_value_get_string (bro_json_value_at (list, i), ""));
  return g_string_free (s, FALSE);
}

static gboolean
same_lines (GPtrArray *failures, const char *id, const char *what, BroJsonValue *want, GPtrArray *got)
{
  g_autofree char *w = joined_json (want);
  g_autofree char *g = joined (got);
  return bro_test_same_string (failures, id, what, w, g);
}

static const char *
op_name (BroJsonValue *op, BroJsonValue **arg)
{
  /* An op is an object with one member: its name and its argument. */
  g_assert_true (op->kind == BRO_JSON_VALUE_OBJECT && op->keys->len == 1);
  *arg = g_ptr_array_index (op->items, 0);
  return g_ptr_array_index (op->keys, 0);
}

/* ---- the event bus ------------------------------------------------------------------------------------------- */

typedef struct {
  TestManualClock *clock;
  BroEventBus *bus;
  GHashTable *subs;     /* name → BroSubscription */
  GHashTable *received; /* name → GPtrArray of char * */
  GHashTable *streams;  /* name → BroEventStream */
  GHashTable *read;     /* name → GPtrArray of char * */
  GPtrArray *published; /* of BroEngineEvent */
} BusHarness;

static void run_bus_ops (BusHarness *h, BroJsonValue *ops);

static char *
event_line (const BroEngineEvent *e)
{
  return g_strdup_printf ("%" G_GINT64_FORMAT " %s", e->seq, bro_engine_event_type_to_wire (e->type));
}

typedef struct {
  BusHarness *h;
  char *name;
  BroJsonValue *on; /* nullable */
} Subscriber;

static void
subscriber_free (gpointer data)
{
  Subscriber *s = data;
  g_free (s->name);
  g_clear_pointer (&s->on, bro_json_value_unref);
  g_free (s);
}

static void
subscriber_got (BroEngineEvent *e, gpointer data)
{
  Subscriber *s = data;
  g_ptr_array_add (g_hash_table_lookup (s->h->received, s->name), event_line (e));
  if (s->on && g_strcmp0 (bro_json_value_get_string (bro_json_value_member (s->on, "type"), ""), bro_engine_event_type_to_wire (e->type)) == 0)
    run_bus_ops (s->h, bro_json_value_member (s->on, "do"));
}

static void
run_bus_ops (BusHarness *h, BroJsonValue *ops)
{
  for (guint i = 0; i < bro_json_value_length (ops); i++)
    {
      BroJsonValue *arg;
      const char *name = op_name (bro_json_value_at (ops, i), &arg);
      if (strcmp (name, "publish") == 0)
        {
          BroEngineEventType t;
          BroJsonValue *data = bro_json_value_member (arg, "data");
          g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
          g_assert_true (bro_engine_event_type_from_wire (bro_json_value_get_string (bro_json_value_member (arg, "type"), ""), &t));
          g_ptr_array_add (h->published, bro_event_bus_publish (h->bus, t, data ? data : empty));
        }
      else if (strcmp (name, "advance") == 0)
        manual_advance (h->clock, bro_json_value_get_integer (arg, 0), NULL, NULL);
      else if (strcmp (name, "subscribe") == 0)
        {
          const char *as = bro_json_value_get_string (bro_json_value_member (arg, "as"), "");
          g_autoptr (GArray) types = types_of (bro_json_value_member (arg, "types"));
          Subscriber *s = g_new0 (Subscriber, 1);
          BroJsonValue *on = bro_json_value_member (arg, "onReceive");
          s->h = h;
          s->name = g_strdup (as);
          s->on = on ? bro_json_value_ref (on) : NULL;
          g_hash_table_replace (h->received, g_strdup (as), g_ptr_array_new_with_free_func (g_free));
          g_hash_table_replace (h->subs, g_strdup (as),
                                bro_event_bus_subscribe (h->bus, (BroEngineEventType *) types->data, types->len, subscriber_got, s, subscriber_free));
        }
      else if (strcmp (name, "unsubscribe") == 0)
        bro_subscription_cancel (g_hash_table_lookup (h->subs, bro_json_value_get_string (arg, "")));
      else if (strcmp (name, "stream") == 0)
        {
          const char *as = bro_json_value_get_string (bro_json_value_member (arg, "as"), "");
          BroJsonValue *after = bro_json_value_member (arg, "after");
          g_autoptr (GArray) types = types_of (bro_json_value_member (arg, "types"));
          g_hash_table_replace (h->streams, g_strdup (as),
                                bro_event_bus_stream (h->bus, after != NULL, after ? bro_json_value_get_integer (after, 0) : 0,
                                                      (BroEngineEventType *) types->data, types->len));
          g_hash_table_replace (h->read, g_strdup (as), g_ptr_array_new_with_free_func (g_free));
        }
      else if (strcmp (name, "read") == 0)
        {
          const char *as = bro_json_value_get_string (arg, "");
          BroEventStream *s = g_hash_table_lookup (h->streams, as);
          GPtrArray *log = g_hash_table_lookup (h->read, as);
          BroStreamItem *item;
          while ((item = bro_event_stream_poll (s)))
            {
              g_ptr_array_add (log, item->kind == BRO_STREAM_ITEM_EVENT ? event_line (item->event) : g_strdup ("resync"));
              bro_stream_item_free (item);
            }
          if (bro_event_stream_is_closed (s))
            g_ptr_array_add (log, g_strdup ("end"));
        }
      else if (strcmp (name, "close") == 0)
        bro_event_stream_close (g_hash_table_lookup (h->streams, bro_json_value_get_string (arg, "")));
      else
        g_error ("unknown op %s", name);
    }
}

static gboolean
compare_types (GPtrArray *failures, const char *id, BroJsonValue *expect)
{
  g_autoptr (GPtrArray) visible = g_ptr_array_new ();
  g_autoptr (GPtrArray) internal = g_ptr_array_new ();
  g_autoptr (BroJsonValue) schema = bro_test_fixture_json ("../schema/events.json");
  BroJsonValue *one_of = bro_json_value_member (schema, "oneOf");
  g_autoptr (GPtrArray) consts = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *got = NULL;
  g_autofree char *want = NULL;
  for (guint t = 0; t < BRO_ENGINE_EVENT_TYPE_COUNT; t++)
    g_ptr_array_add (bro_engine_event_type_is_internal (t) ? internal : visible, (gpointer) bro_engine_event_type_to_wire (t));
  same_lines (failures, id, "client-visible", bro_json_value_member (expect, "clientVisible"), visible);
  same_lines (failures, id, "internal", bro_json_value_member (expect, "internal"), internal);
  for (guint i = 0; i < bro_json_value_length (one_of); i++)
    {
      const char *ref = bro_json_value_get_string (bro_json_value_member (bro_json_value_at (one_of, i), "$ref"), "");
      BroJsonValue *def = bro_json_value_member (bro_json_value_member (schema, "$defs"), strrchr (ref, '/') + 1);
      BroJsonValue *type = bro_json_value_member (bro_json_value_member (bro_json_value_member (def, "properties"), "type"), "const");
      g_ptr_array_add (consts, g_strdup (bro_json_value_get_string (type, "")));
    }
  got = joined (visible);
  want = joined (consts);
  bro_test_same_string (failures, id, "events.json", want, got);
  return TRUE;
}

static gboolean
bus_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  BusHarness h = { 0 };
  BroJsonValue *v;
  if (strcmp (id, "event-types") == 0)
    return compare_types (failures, id, expect);
  h.clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
  h.bus = bro_event_bus_new (BRO_CLOCK (h.clock), bro_json_value_get_integer (bro_json_value_member (given, "firstSeq"), 1),
                             bro_json_value_get_integer (bro_json_value_member (given, "retain"), 1024),
                             bro_json_value_get_integer (bro_json_value_member (given, "streamCapacity"), 1024));
  h.subs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  h.received = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  h.streams = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  h.read = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  h.published = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_engine_event_unref);
  run_bus_ops (&h, bro_json_value_member (given, "ops"));
  if ((v = bro_json_value_member (expect, "lastSeq")))
    {
      g_autofree char *w = g_strdup_printf ("%" G_GINT64_FORMAT, bro_json_value_get_integer (v, -1));
      g_autofree char *g = g_strdup_printf ("%" G_GINT64_FORMAT, bro_event_bus_last_seq (h.bus));
      bro_test_same_string (failures, id, "lastSeq", w, g);
    }
  if ((v = bro_json_value_member (expect, "events")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      for (guint i = 0; i < h.published->len; i++)
        bro_json_value_append (got, bro_engine_event_json (g_ptr_array_index (h.published, i)));
      bro_test_same_json (failures, id, "events", v, got);
    }
  for (const char *key = "received"; key; key = strcmp (key, "received") == 0 ? "read" : NULL)
    if ((v = bro_json_value_member (expect, key)))
      for (guint i = 0; i < v->keys->len; i++)
        {
          const char *name = g_ptr_array_index (v->keys, i);
          g_autofree char *what = g_strdup_printf ("%s %s", key, name);
          same_lines (failures, id, what, bro_json_value_member (v, name),
                      g_hash_table_lookup (strcmp (key, "received") == 0 ? h.received : h.read, name));
        }
  g_ptr_array_unref (h.published);
  g_hash_table_unref (h.subs);
  g_hash_table_unref (h.received);
  g_hash_table_unref (h.streams);
  g_hash_table_unref (h.read);
  g_object_unref (h.bus);
  g_object_unref (h.clock);
  return TRUE;
}

static void
test_event_bus (void)
{
  bro_test_run_cases ("engine/event-bus.cases.json", bus_case);
}

static void
test_stream_next_waits (void)
{
  g_autoptr (TestManualClock) clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
  g_autoptr (BroEventBus) bus = bro_event_bus_new (BRO_CLOCK (clock), 1, 16, 16);
  g_autoptr (BroEventStream) stream = bro_event_bus_stream (bus, FALSE, 0, NULL, 0);
  g_autoptr (BroJsonValue) data = bro_json_value_new_object ();
  g_autoptr (GThread) reader = g_thread_new ("reader", (GThreadFunc) bro_event_stream_next, stream);
  BroStreamItem *item;
  g_usleep (20000);
  bro_engine_event_unref (bro_event_bus_publish (bus, BRO_ENGINE_EVENT_TYPE_JOB_CHANGED, data));
  item = g_thread_join (g_steal_pointer (&reader));
  g_assert_nonnull (item);
  g_assert_cmpint (item->kind, ==, BRO_STREAM_ITEM_EVENT);
  g_assert_cmpint (item->event->seq, ==, 1);
  bro_stream_item_free (item);
  reader = g_thread_new ("reader", (GThreadFunc) bro_event_stream_next, stream);
  g_usleep (20000);
  bro_event_stream_close (stream);
  g_assert_null (g_thread_join (g_steal_pointer (&reader)));
}

/* ---- the engine clock and the coalescer, on a real executor --------------------------------------------------- */

typedef struct {
  TestManualClock *clock;
  BroEngineExecutor *executor;
  BroEngineClock *engine_clock;
  GHashTable *timers; /* name → BroEngineTimer; on the executor */
  GPtrArray *fired;   /* on the executor */
  gboolean all_on_executor;
  BroCoalescer *coalescer;
} World;

static void
settle (gpointer data)
{
  World *w = data;
  if (!bro_engine_executor_is_current (w->executor))
    bro_engine_executor_barrier (w->executor);
}

static void
world_init (World *w)
{
  w->clock = g_object_new (TEST_TYPE_MANUAL_CLOCK, NULL);
  w->executor = bro_engine_executor_new ("engine-test");
  w->engine_clock = bro_engine_clock_new (BRO_CLOCK (w->clock), w->executor);
  w->timers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  w->fired = g_ptr_array_new_with_free_func (g_free);
  w->all_on_executor = TRUE;
}

static void
clear_on_executor (gpointer data)
{
  World *w = data;
  GHashTableIter it;
  gpointer timer;
  /* A repeating timer runs until it is cancelled, referenced or not (the Clock port's rule). */
  g_hash_table_iter_init (&it, w->timers);
  while (g_hash_table_iter_next (&it, NULL, &timer))
    bro_engine_timer_cancel (timer);
  g_hash_table_remove_all (w->timers);
  g_clear_object (&w->coalescer);
}

static void
world_clear (World *w)
{
  bro_engine_executor_post (w->executor, clear_on_executor, w, NULL);
  bro_engine_executor_stop (w->executor);
  g_hash_table_unref (w->timers);
  g_ptr_array_unref (w->fired);
  g_object_unref (w->engine_clock);
  g_object_unref (w->executor);
  g_object_unref (w->clock);
}

typedef struct {
  World *w;
  BroJsonValue *ops;
  const char *op;
  BroJsonValue *arg;
} OnExecutor;

static void run_clock_ops (World *w, BroJsonValue *ops);

static void
run_clock_ops_action (gpointer data)
{
  OnExecutor *o = data;
  run_clock_ops (o->w, o->ops);
}

/* Runs @ops on the executor and waits for them. */
static void
on_executor (World *w, BroJsonValue *ops)
{
  OnExecutor o = { w, ops };
  if (bro_engine_executor_is_current (w->executor))
    run_clock_ops (w, ops);
  else
    {
      bro_engine_executor_post (w->executor, run_clock_ops_action, &o, NULL);
      bro_engine_executor_barrier (w->executor);
    }
}

typedef struct {
  World *w;
  char *name;
  gint64 cancel_after;
  gint64 count;
} Handler;

static void
handler_free (gpointer data)
{
  Handler *h = data;
  g_free (h->name);
  g_free (h);
}

static void
timer_fired (gpointer data)
{
  Handler *h = data;
  h->w->all_on_executor &= bro_engine_executor_is_current (h->w->executor);
  g_ptr_array_add (h->w->fired, g_strdup (h->name));
  if (++h->count == h->cancel_after)
    bro_engine_timer_cancel (g_hash_table_lookup (h->w->timers, h->name));
}

static void
run_clock_ops (World *w, BroJsonValue *ops)
{
  for (guint i = 0; i < bro_json_value_length (ops); i++)
    {
      BroJsonValue *op = bro_json_value_at (ops, i), *arg;
      const char *name = op_name (op, &arg);
      gboolean here = bro_engine_executor_is_current (w->executor);
      if (strcmp (name, "advance") == 0)
        manual_advance (w->clock, bro_json_value_get_integer (arg, 0), settle, w);
      else if (strcmp (name, "onExecutor") == 0)
        on_executor (w, arg);
      else if (!here)
        {
          /* timer and cancel run on the executor */
          g_autoptr (BroJsonValue) one = bro_json_value_new_array ();
          bro_json_value_append (one, bro_json_value_ref (op));
          on_executor (w, one);
        }
      else if (strcmp (name, "timer") == 0)
        {
          BroTimerSchedule schedule = { 0 };
          BroJsonValue *at = bro_json_value_member (arg, "at");
          Handler *h = g_new0 (Handler, 1);
          h->w = w;
          h->name = g_strdup (bro_json_value_get_string (bro_json_value_member (arg, "as"), ""));
          h->cancel_after = bro_json_value_get_integer (bro_json_value_member (arg, "cancelSelfAfter"), -1);
          if (at)
            {
              schedule.kind = BRO_TIMER_SCHEDULE_AT;
              schedule.instant.unix_milliseconds = START_MS + bro_json_value_get_integer (at, 0);
            }
          else
            {
              schedule.kind = BRO_TIMER_SCHEDULE_EVERY;
              schedule.interval.seconds = bro_json_value_get_number (bro_json_value_member (arg, "every"), 0);
            }
          g_hash_table_replace (w->timers, g_strdup (h->name),
                                bro_engine_clock_timer (w->engine_clock, &schedule, timer_fired, h, handler_free));
        }
      else if (strcmp (name, "cancel") == 0)
        bro_engine_timer_cancel (g_hash_table_lookup (w->timers, bro_json_value_get_string (arg, "")));
      else
        g_error ("unknown op %s", name);
    }
}

static gboolean
clock_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  World w = { 0 };
  BroJsonValue *v;
  world_init (&w);
  run_clock_ops (&w, bro_json_value_member (given, "ops"));
  bro_engine_executor_barrier (w.executor);
  same_lines (failures, id, "fired", bro_json_value_member (expect, "fired"), w.fired);
  if ((v = bro_json_value_member (expect, "allOnExecutor")) && bro_json_value_get_bool (v, FALSE) != w.all_on_executor)
    bro_test_fail (failures, id, "allOnExecutor: expected %d", bro_json_value_get_bool (v, FALSE));
  if ((v = bro_json_value_member (expect, "now")))
    {
      g_autofree char *now = bro_instant_format (bro_engine_clock_now (w.engine_clock));
      bro_test_same_string (failures, id, "now", bro_json_value_get_string (v, ""), now);
    }
  if ((v = bro_json_value_member (expect, "monotonicSeconds")) && bro_json_value_get_number (v, -1) != bro_engine_clock_monotonic (w.engine_clock).seconds)
    bro_test_fail (failures, id, "monotonic: expected %g, got %g", bro_json_value_get_number (v, -1), bro_engine_clock_monotonic (w.engine_clock).seconds);
  world_clear (&w);
  return TRUE;
}

static void
test_engine_clock (void)
{
  bro_test_run_cases ("engine/engine-clock.cases.json", clock_case);
}

static void
emitted (const char *key, BroJsonValue *value, gpointer data)
{
  World *w = data;
  gint64 ms = (gint64) (bro_engine_clock_monotonic (w->engine_clock).seconds * 1000 + 0.5);
  g_ptr_array_add (w->fired, g_strdup_printf ("%" G_GINT64_FORMAT " %s %s", ms, key, bro_json_value_get_string (value, "")));
}

static void
coalescer_op (gpointer data)
{
  OnExecutor *o = data;
  World *w = o->w;
  if (strcmp (o->op, "new") == 0)
    {
      BroDuration interval = { 0.1 };
      w->coalescer = bro_coalescer_new (w->engine_clock, interval, emitted, w, NULL);
    }
  else if (strcmp (o->op, "offer") == 0)
    bro_coalescer_offer (w->coalescer, bro_json_value_get_string (bro_json_value_member (o->arg, "key"), ""),
                         bro_json_value_member (o->arg, "value"));
  else if (strcmp (o->op, "flush") == 0)
    bro_coalescer_flush (w->coalescer, bro_json_value_get_string (o->arg, ""));
  else if (strcmp (o->op, "drop") == 0)
    bro_coalescer_drop (w->coalescer, bro_json_value_get_string (o->arg, ""));
  else
    g_error ("unknown op %s", o->op);
}

static void
coalescer_on_executor (World *w, const char *op, BroJsonValue *arg)
{
  OnExecutor o = { w, NULL, op, arg };
  bro_engine_executor_post (w->executor, coalescer_op, &o, NULL);
  bro_engine_executor_barrier (w->executor);
}

static gboolean
coalescer_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  World w = { 0 };
  BroJsonValue *ops = bro_json_value_member (given, "ops");
  world_init (&w);
  coalescer_on_executor (&w, "new", NULL);
  for (guint i = 0; i < bro_json_value_length (ops); i++)
    {
      BroJsonValue *arg;
      const char *name = op_name (bro_json_value_at (ops, i), &arg);
      if (strcmp (name, "advance") == 0)
        manual_advance (w.clock, bro_json_value_get_integer (arg, 0), settle, &w);
      else
        coalescer_on_executor (&w, name, arg);
    }
  bro_engine_executor_barrier (w.executor);
  same_lines (failures, id, "emitted", bro_json_value_member (expect, "emitted"), w.fired);
  world_clear (&w);
  return TRUE;
}

static void
test_coalescer (void)
{
  bro_test_run_cases ("engine/coalescer.cases.json", coalescer_case);
}

/* ---- the executor --------------------------------------------------------------------------------------------- */

typedef struct {
  BroEngineExecutor *executor;
  GMutex lock;
  GPtrArray *log; /* of char *, under the lock */
  GHashTable *threads;
  gboolean all_current;
  int count;
  GMutex hold_lock;
  GCond hold_cond;
  gboolean held;
  int workers_started;
  int workers_wanted;
  gboolean all_started;
} Probe;

static void
log_line (Probe *p, char *line)
{
  g_mutex_lock (&p->lock);
  g_ptr_array_add (p->log, line);
  g_mutex_unlock (&p->lock);
}

typedef struct {
  Probe *p;
  char *line;
} Line;

static void
line_free (gpointer data)
{
  Line *l = data;
  g_free (l->line);
  g_free (l);
}

static void
log_action (gpointer data)
{
  Line *l = data;
  log_line (l->p, g_strdup (l->line));
}

static void
post_line (Probe *p, const char *line)
{
  Line *l = g_new0 (Line, 1);
  l->p = p;
  l->line = g_strdup (line);
  g_assert_true (bro_engine_executor_post (p->executor, log_action, l, line_free));
}

typedef struct {
  Probe *p;
  int thread;
  int each;
} Poster;

static gpointer
poster (gpointer data)
{
  Poster *ps = data;
  for (int i = 0; i < ps->each; i++)
    {
      g_autofree char *line = g_strdup_printf ("%d %d", ps->thread, i);
      post_line (ps->p, line);
    }
  return NULL;
}

static void
note_thread (gpointer data)
{
  Probe *p = data;
  g_hash_table_add (p->threads, g_thread_self ());
  p->all_current &= bro_engine_executor_is_current (p->executor);
}

static void
first_action (gpointer data)
{
  Probe *p = data;
  log_line (p, g_strdup ("first"));
  post_line (p, "posted by first");
  log_line (p, g_strdup ("first ends"));
}

typedef struct {
  Probe *p;
  gboolean on_executor_during_work;
  gboolean done_on_executor;
  gint64 result;
  BroBroError *fail_with;
  BroBroError *got;
  gboolean finished;
} Work;

static gboolean
work_answer (gpointer data, BroBroError **error)
{
  Work *w = data;
  w->on_executor_during_work = bro_engine_executor_is_current (w->p->executor);
  if (w->fail_with)
    {
      *error = bro_bro_error_copy (w->fail_with);
      return FALSE;
    }
  w->result = 42;
  return TRUE;
}

static void
work_done (gpointer data, const BroBroError *error)
{
  Work *w = data;
  w->done_on_executor = bro_engine_executor_is_current (w->p->executor);
  w->got = error ? bro_bro_error_copy (error) : NULL;
  g_mutex_lock (&w->p->lock);
  w->finished = TRUE;
  g_mutex_unlock (&w->p->lock);
}

static gboolean
work_meets_the_others (gpointer data, BroBroError **error)
{
  Probe *p = data;
  gint64 until = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  g_mutex_lock (&p->hold_lock);
  p->workers_started++;
  g_cond_broadcast (&p->hold_cond);
  while (p->workers_started < p->workers_wanted && g_get_monotonic_time () < until)
    g_cond_wait_until (&p->hold_cond, &p->hold_lock, until);
  if (p->workers_started < p->workers_wanted)
    p->all_started = FALSE;
  g_mutex_unlock (&p->hold_lock);
  return TRUE;
}

static void
nothing_done (gpointer data, const BroBroError *error)
{
}

static void
slow_action (gpointer data)
{
  Probe *p = data;
  g_usleep (50000);
  log_line (p, g_strdup ("slow done"));
}

static void
hold_action (gpointer data)
{
  Probe *p = data;
  g_mutex_lock (&p->hold_lock);
  while (!p->held)
    g_cond_wait (&p->hold_cond, &p->hold_lock);
  g_mutex_unlock (&p->hold_lock);
}

static void
count_action (gpointer data)
{
  Probe *p = data;
  p->count++;
}

static void
nothing (gpointer data)
{
}

static gpointer
stopper (gpointer data)
{
  bro_engine_executor_stop (data);
  return NULL;
}

static gboolean
executor_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  Probe p = { 0 };
  gboolean known = TRUE;
  p.executor = bro_engine_executor_new ("engine-test");
  g_mutex_init (&p.lock);
  g_mutex_init (&p.hold_lock);
  g_cond_init (&p.hold_cond);
  p.log = g_ptr_array_new_with_free_func (g_free);
  p.threads = g_hash_table_new (NULL, NULL);
  p.all_current = TRUE;
  if (strcmp (id, "in-order") == 0)
    {
      gint64 n = bro_json_value_get_integer (bro_json_value_member (given, "actions"), 0);
      g_autoptr (GPtrArray) want = g_ptr_array_new_with_free_func (g_free);
      for (gint64 i = 0; i < n; i++)
        {
          g_autofree char *line = g_strdup_printf ("%" G_GINT64_FORMAT, i);
          post_line (&p, line);
          g_ptr_array_add (want, g_strdup (line));
        }
      bro_engine_executor_barrier (p.executor);
      {
        g_autofree char *w = joined (want), *g = joined (p.log);
        bro_test_same_string (failures, id, "order", w, g);
      }
    }
  else if (strcmp (id, "in-order-per-thread") == 0)
    {
      int threads = bro_json_value_get_integer (bro_json_value_member (given, "threads"), 0);
      int each = bro_json_value_get_integer (bro_json_value_member (given, "actionsEach"), 0);
      Poster *ps = g_new0 (Poster, threads);
      GThread **t = g_new0 (GThread *, threads);
      for (int i = 0; i < threads; i++)
        {
          ps[i] = (Poster) { &p, i, each };
          t[i] = g_thread_new ("poster", poster, &ps[i]);
        }
      for (int i = 0; i < threads; i++)
        g_thread_join (t[i]);
      bro_engine_executor_barrier (p.executor);
      if ((gint64) p.log->len != bro_json_value_get_integer (bro_json_value_member (expect, "ran"), -1))
        bro_test_fail (failures, id, "ran %u", p.log->len);
      for (int i = 0; i < threads; i++)
        {
          int next = 0;
          for (guint k = 0; k < p.log->len; k++)
            {
              int ti, n;
              if (sscanf (g_ptr_array_index (p.log, k), "%d %d", &ti, &n) == 2 && ti == i && n != next++)
                bro_test_fail (failures, id, "thread %d: action %d ran as number %d", i, n, next - 1);
            }
        }
      g_free (ps);
      g_free (t);
    }
  else if (strcmp (id, "one-thread-of-its-own") == 0)
    {
      gint64 n = bro_json_value_get_integer (bro_json_value_member (given, "actions"), 0);
      for (gint64 i = 0; i < n; i++)
        bro_engine_executor_post (p.executor, note_thread, &p, NULL);
      bro_engine_executor_barrier (p.executor);
      if (p.all_current != bro_json_value_get_bool (bro_json_value_member (expect, "isCurrentInActions"), FALSE))
        bro_test_fail (failures, id, "isCurrent in actions");
      if (bro_engine_executor_is_current (p.executor) != bro_json_value_get_bool (bro_json_value_member (expect, "isCurrentOutside"), TRUE))
        bro_test_fail (failures, id, "isCurrent outside");
      if (g_hash_table_size (p.threads) != bro_json_value_get_integer (bro_json_value_member (expect, "threads"), -1)
          || g_hash_table_contains (p.threads, g_thread_self ()))
        bro_test_fail (failures, id, "threads: %u", g_hash_table_size (p.threads));
    }
  else if (strcmp (id, "posted-on-the-executor-runs-later") == 0)
    {
      bro_engine_executor_post (p.executor, hold_action, &p, NULL);
      bro_engine_executor_post (p.executor, first_action, &p, NULL);
      post_line (&p, "second");
      g_mutex_lock (&p.hold_lock);
      p.held = TRUE;
      g_cond_broadcast (&p.hold_cond);
      g_mutex_unlock (&p.hold_lock);
      bro_engine_executor_barrier (p.executor);
      bro_engine_executor_barrier (p.executor);
      same_lines (failures, id, "order", bro_json_value_member (expect, "order"), p.log);
    }
  else if (strcmp (id, "worker-runs-off-the-executor") == 0 || strcmp (id, "worker-error") == 0)
    {
      Work *w = g_new0 (Work, 1);
      BroJsonValue *e = bro_json_value_member (given, "error");
      gboolean finished = FALSE;
      w->p = &p;
      if (e)
        w->fail_with = bro_bro_error_new (bro_json_value_get_string (bro_json_value_member (e, "code"), ""),
                                          bro_json_value_ref (bro_json_value_member (e, "params")), NULL);
      bro_engine_executor_worker (p.executor, work_answer, work_done, w, NULL);
      for (int tries = 0; tries < 1000 && !finished; tries++)
        {
          bro_engine_executor_barrier (p.executor);
          g_mutex_lock (&p.lock);
          finished = w->finished;
          g_mutex_unlock (&p.lock);
          if (!finished)
            g_usleep (1000);
        }
      if (!finished)
        bro_test_fail (failures, id, "the work never came back");
      else if (!e)
        {
          if (w->on_executor_during_work || !w->done_on_executor || w->result != 42 || w->got)
            bro_test_fail (failures, id, "work on the executor %d, done on it %d, result %" G_GINT64_FORMAT,
                           w->on_executor_during_work, w->done_on_executor, w->result);
        }
      else
        {
          BroJsonValue *want = bro_json_value_member (expect, "error");
          if (!w->done_on_executor || !w->got)
            bro_test_fail (failures, id, "no error on the executor");
          else
            {
              bro_test_same_string (failures, id, "code", bro_json_value_get_string (bro_json_value_member (want, "code"), ""), w->got->code);
              bro_test_same_json (failures, id, "params", bro_json_value_member (want, "params"), w->got->params);
            }
        }
      g_clear_pointer (&w->fail_with, bro_bro_error_free);
      g_clear_pointer (&w->got, bro_bro_error_free);
      g_free (w);
    }
  else if (strcmp (id, "workers-in-parallel") == 0)
    {
      p.workers_wanted = bro_json_value_get_integer (bro_json_value_member (given, "workers"), 0);
      p.all_started = TRUE;
      for (int i = 0; i < p.workers_wanted; i++)
        bro_engine_executor_worker (p.executor, work_meets_the_others, nothing_done, &p, NULL);
      g_mutex_lock (&p.hold_lock);
      while (p.workers_started < p.workers_wanted)
        g_cond_wait (&p.hold_cond, &p.hold_lock);
      g_mutex_unlock (&p.hold_lock);
      bro_engine_executor_barrier (p.executor);
      g_usleep (20000);
      if (!p.all_started)
        bro_test_fail (failures, id, "a worker ended before the others started");
    }
  else if (strcmp (id, "barrier-waits") == 0)
    {
      bro_engine_executor_post (p.executor, slow_action, &p, NULL);
      bro_engine_executor_barrier (p.executor);
      if (p.log->len != 1)
        bro_test_fail (failures, id, "the barrier came back before the slow action ended");
    }
  else if (strcmp (id, "activity") == 0)
    {
      gint64 behind = bro_json_value_get_integer (bro_json_value_member (given, "queuedBehind"), 0);
      gint64 until = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
      int waiting, afterwards;
      p.workers_wanted = 2; /* the worker waits until the test counts as a second "worker" */
      bro_engine_executor_worker (p.executor, work_meets_the_others, nothing_done, &p, NULL);
      g_mutex_lock (&p.hold_lock);
      while (p.workers_started < 1)
        g_cond_wait (&p.hold_cond, &p.hold_lock);
      g_mutex_unlock (&p.hold_lock);
      bro_engine_executor_post (p.executor, hold_action, &p, NULL);
      while (bro_engine_executor_activity (p.executor) > 1) /* the hold action has started */
        g_usleep (1000);
      for (gint64 i = 0; i < behind; i++)
        bro_engine_executor_post (p.executor, nothing, NULL, NULL);
      waiting = bro_engine_executor_activity (p.executor);
      if (waiting != bro_json_value_get_integer (bro_json_value_member (expect, "whileWaiting"), -1))
        bro_test_fail (failures, id, "activity while waiting: %d", waiting);
      g_mutex_lock (&p.hold_lock);
      p.workers_started++; /* lets the worker go */
      p.held = TRUE;
      g_cond_broadcast (&p.hold_cond);
      g_mutex_unlock (&p.hold_lock);
      while (bro_engine_executor_activity (p.executor) > 0 && g_get_monotonic_time () < until)
        g_usleep (1000);
      bro_engine_executor_barrier (p.executor);
      afterwards = bro_engine_executor_activity (p.executor);
      if (afterwards != bro_json_value_get_integer (bro_json_value_member (expect, "afterwards"), -1))
        bro_test_fail (failures, id, "activity afterwards: %d", afterwards);
    }
  else if (strcmp (id, "stop-runs-what-is-queued") == 0)
    {
      gint64 n = bro_json_value_get_integer (bro_json_value_member (given, "actions"), 0);
      GThread *t;
      int before;
      gboolean accepted;
      bro_engine_executor_post (p.executor, hold_action, &p, NULL);
      for (gint64 i = 0; i < n; i++)
        bro_engine_executor_post (p.executor, count_action, &p, NULL);
      t = g_thread_new ("stopper", stopper, p.executor);
      while (bro_engine_executor_post (p.executor, nothing, NULL, NULL))
        g_usleep (1000);
      g_mutex_lock (&p.hold_lock);
      p.held = TRUE;
      g_cond_broadcast (&p.hold_cond);
      g_mutex_unlock (&p.hold_lock);
      g_thread_join (t);
      before = p.count;
      accepted = bro_engine_executor_post (p.executor, count_action, &p, NULL);
      g_usleep (20000);
      if (before != bro_json_value_get_integer (bro_json_value_member (expect, "ranBeforeStop"), -1))
        bro_test_fail (failures, id, "ran %d before the stop ended", before);
      if (accepted != bro_json_value_get_bool (bro_json_value_member (expect, "postAfterStop"), TRUE))
        bro_test_fail (failures, id, "post after stop: %d", accepted);
      if (p.count - before != bro_json_value_get_integer (bro_json_value_member (expect, "ranAfterStop"), -1))
        bro_test_fail (failures, id, "ran %d after stop", p.count - before);
    }
  else
    known = FALSE;
  bro_engine_executor_stop (p.executor);
  g_object_unref (p.executor);
  g_ptr_array_unref (p.log);
  g_hash_table_unref (p.threads);
  g_mutex_clear (&p.lock);
  g_mutex_clear (&p.hold_lock);
  g_cond_clear (&p.hold_cond);
  return known;
}

static void
test_engine_executor (void)
{
  bro_test_run_cases ("engine/engine-executor.cases.json", executor_case);
}

#include "test-scheduler.inc"
#include "test-pipeline.inc"
#include "test-steps.inc"
#include "test-services.inc"
#include "test-library.inc"
#include "test-recovery.inc"

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/engine/event-bus", test_event_bus);
  g_test_add_func ("/engine/stream-next-waits", test_stream_next_waits);
  g_test_add_func ("/engine/engine-clock", test_engine_clock);
  g_test_add_func ("/engine/coalescer", test_coalescer);
  g_test_add_func ("/engine/executor", test_engine_executor);
  g_test_add_func ("/engine/scheduler", test_scheduler);
  g_test_add_func ("/engine/lease-snapshot", test_lease_snapshot);
  g_test_add_func ("/engine/pipeline-runner", test_pipeline_runner);
  g_test_add_func ("/engine/job-planner", test_job_planner);
  g_test_add_func ("/engine/video-steps", test_video_steps);
  g_test_add_func ("/engine/problem-service", test_problem_service);
  g_test_add_func ("/engine/config-service", test_config_service);
  g_test_add_func ("/engine/library-service", test_library_service);
  g_test_add_func ("/engine/recovery-service", test_recovery_service);
  g_test_add_func ("/engine/sha256-hasher", test_sha256_hasher);
  return g_test_run ();
}
