/* bro-coalescer.c */
#include "bro-coalescer.h"

#include <math.h>

typedef struct {
  gboolean sent;
  gint64 last_sent_ms;
  BroJsonValue *waiting;
  BroEngineTimer *timer;
} Key;

struct _BroCoalescer {
  GObject parent_instance;
  BroEngineClock *clock;
  gint64 interval_ms;
  BroCoalescerEmit emit;
  gpointer data;
  GDestroyNotify destroy;
  GHashTable *keys; /* char * → Key */
};

G_DEFINE_FINAL_TYPE (BroCoalescer, bro_coalescer, G_TYPE_OBJECT)

static void
key_free (gpointer data)
{
  Key *k = data;
  if (k->timer)
    {
      bro_engine_timer_cancel (k->timer);
      g_object_unref (k->timer);
    }
  g_clear_pointer (&k->waiting, bro_json_value_unref);
  g_free (k);
}

BroCoalescer *
bro_coalescer_new (BroEngineClock *clock, BroDuration interval, BroCoalescerEmit emit, gpointer data,
                   GDestroyNotify destroy)
{
  BroCoalescer *self = g_object_new (BRO_TYPE_COALESCER, NULL);
  self->clock = g_object_ref (clock);
  self->interval_ms = (gint64) llround (interval.seconds * 1000);
  self->emit = emit;
  self->data = data;
  self->destroy = destroy;
  return self;
}

static gint64
now_ms (BroCoalescer *self)
{
  return (gint64) llround (bro_engine_clock_monotonic (self->clock).seconds * 1000);
}

static void
send (BroCoalescer *self, const char *key, Key *k, BroJsonValue *value, gint64 now)
{
  if (k->timer)
    {
      bro_engine_timer_cancel (k->timer);
      g_clear_object (&k->timer);
    }
  k->waiting = NULL; /* @value is the caller's (it was the waiting one, or an offered one) */
  k->sent = TRUE;
  k->last_sent_ms = now;
  self->emit (key, value, self->data);
  bro_json_value_unref (value);
}

/* What a key's timer holds: the coalescer (not a reference: it cancels its timers when it goes) and the key. */
typedef struct {
  BroCoalescer *self;
  char *key;
} Due;

static void
due_free (gpointer data)
{
  Due *d = data;
  g_free (d->key);
  g_free (d);
}

static void
due (gpointer data)
{
  Due *d = data;
  Key *k = g_hash_table_lookup (d->self->keys, d->key);
  if (!k)
    return;
  g_clear_object (&k->timer);
  if (k->waiting)
    send (d->self, d->key, k, k->waiting, now_ms (d->self));
}

void
bro_coalescer_offer (BroCoalescer *self, const char *key, BroJsonValue *value)
{
  Key *k = g_hash_table_lookup (self->keys, key);
  gint64 now = now_ms (self);
  if (!k)
    {
      k = g_new0 (Key, 1);
      g_hash_table_insert (self->keys, g_strdup (key), k);
    }
  if (!k->waiting && (!k->sent || now - k->last_sent_ms >= self->interval_ms))
    {
      send (self, key, k, bro_json_value_ref (value), now);
      return;
    }
  g_clear_pointer (&k->waiting, bro_json_value_unref);
  k->waiting = bro_json_value_ref (value);
  if (!k->timer)
    {
      gint64 at = (k->sent ? k->last_sent_ms : now) + self->interval_ms;
      BroTimerSchedule schedule = { BRO_TIMER_SCHEDULE_AT };
      Due *d = g_new0 (Due, 1);
      d->self = self;
      d->key = g_strdup (key);
      schedule.instant.unix_milliseconds = bro_engine_clock_now (self->clock).unix_milliseconds + MAX (0, at - now);
      k->timer = bro_engine_clock_timer (self->clock, &schedule, due, d, due_free);
    }
}

void
bro_coalescer_flush (BroCoalescer *self, const char *key)
{
  Key *k = g_hash_table_lookup (self->keys, key);
  if (k && k->waiting)
    send (self, key, k, k->waiting, now_ms (self));
}

void
bro_coalescer_drop (BroCoalescer *self, const char *key)
{
  g_hash_table_remove (self->keys, key);
}

static void
bro_coalescer_finalize (GObject *object)
{
  BroCoalescer *self = BRO_COALESCER (object);
  g_hash_table_unref (self->keys);
  g_object_unref (self->clock);
  if (self->destroy)
    self->destroy (self->data);
  G_OBJECT_CLASS (bro_coalescer_parent_class)->finalize (object);
}

static void
bro_coalescer_class_init (BroCoalescerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_coalescer_finalize;
}

static void
bro_coalescer_init (BroCoalescer *self)
{
  self->keys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, key_free);
}
