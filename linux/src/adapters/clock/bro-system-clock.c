/* bro-system-clock.c */
#include "bro-system-clock.h"

#include "bro-message-code.h"

/* ---- timers ---- */

/* What a timer's thread and its handle share. */
typedef struct {
  gatomicrefcount refs;
  GMutex lock;
  GCond cond;
  gboolean cancelled;
  BroTimerSchedule schedule;
  BroTimerFunc handler;
  gpointer data;
  GDestroyNotify destroy;
} TimerState;

static TimerState *
timer_state_ref (TimerState *t)
{
  g_atomic_ref_count_inc (&t->refs);
  return t;
}

static void
timer_state_unref (TimerState *t)
{
  if (!g_atomic_ref_count_dec (&t->refs))
    return;
  if (t->destroy)
    t->destroy (t->data);
  g_mutex_clear (&t->lock);
  g_cond_clear (&t->cond);
  g_free (t);
}

static void
timer_state_cancel (TimerState *t)
{
  g_mutex_lock (&t->lock);
  t->cancelled = TRUE;
  g_cond_broadcast (&t->cond);
  g_mutex_unlock (&t->lock);
}

static gpointer
timer_thread (gpointer data)
{
  TimerState *t = data;
  gint64 due = t->schedule.kind == BRO_TIMER_SCHEDULE_AT
                 ? g_get_monotonic_time () + MAX (0, t->schedule.instant.unix_milliseconds - g_get_real_time () / 1000) * 1000
                 : g_get_monotonic_time () + (gint64) (t->schedule.interval.seconds * G_USEC_PER_SEC);
  g_mutex_lock (&t->lock);
  while (!t->cancelled)
    {
      if (g_cond_wait_until (&t->cond, &t->lock, due) || g_get_monotonic_time () < due)
        continue; /* signalled (cancel) or woken early */
      g_mutex_unlock (&t->lock);
      t->handler (t->data);
      g_mutex_lock (&t->lock);
      if (t->schedule.kind == BRO_TIMER_SCHEDULE_AT)
        break;
      due += MAX ((gint64) (t->schedule.interval.seconds * G_USEC_PER_SEC), 1);
    }
  g_mutex_unlock (&t->lock);
  timer_state_unref (t);
  return NULL;
}

#define BRO_TYPE_SYSTEM_TIMER (bro_system_timer_get_type ())
G_DECLARE_FINAL_TYPE (BroSystemTimer, bro_system_timer, BRO, SYSTEM_TIMER, GObject)

struct _BroSystemTimer {
  GObject parent_instance;
  TimerState *state;
};

static void bro_system_timer_iface_init (BroTimerHandleInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroSystemTimer, bro_system_timer, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_TIMER_HANDLE, bro_system_timer_iface_init))

static void
bro_system_timer_cancel (BroTimerHandle *handle)
{
  timer_state_cancel (BRO_SYSTEM_TIMER (handle)->state);
}

static void
bro_system_timer_finalize (GObject *object)
{
  timer_state_unref (BRO_SYSTEM_TIMER (object)->state); /* the thread keeps its own reference */
  G_OBJECT_CLASS (bro_system_timer_parent_class)->finalize (object);
}

static void
bro_system_timer_class_init (BroSystemTimerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_system_timer_finalize;
}

static void
bro_system_timer_init (BroSystemTimer *self)
{
}

static void
bro_system_timer_iface_init (BroTimerHandleInterface *iface)
{
  iface->cancel = bro_system_timer_cancel;
}

/* ---- the clock ---- */

struct _BroSystemClock {
  GObject parent_instance;
};

static void bro_system_clock_iface_init (BroClockInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroSystemClock, bro_system_clock, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CLOCK, bro_system_clock_iface_init))

BroInstant
bro_system_clock_now (BroSystemClock *self)
{
  return (BroInstant) { g_get_real_time () / 1000 };
}

BroDuration
bro_system_clock_monotonic (BroSystemClock *self)
{
  return (BroDuration) { (double) g_get_monotonic_time () / G_USEC_PER_SEC };
}

typedef struct {
  GMutex lock;
  GCond cond;
  gboolean cancelled;
} Sleeper;

static void
wake_sleeper (gpointer data, gpointer unused)
{
  Sleeper *s = data;
  g_mutex_lock (&s->lock);
  s->cancelled = TRUE;
  g_cond_broadcast (&s->cond);
  g_mutex_unlock (&s->lock);
}

gboolean
bro_system_clock_sleep (BroSystemClock *self, BroDuration duration, BroCancellationToken *cancel, BroBroError **error)
{
  Sleeper s = { 0 };
  gint64 due = g_get_monotonic_time () + (gint64) (MAX (0, duration.seconds) * G_USEC_PER_SEC);
  guint id = 0;
  gboolean cancelled;
  g_mutex_init (&s.lock);
  g_cond_init (&s.cond);
  if (cancel)
    id = bro_cancellation_token_on_cancel (cancel, wake_sleeper, &s, NULL);
  g_mutex_lock (&s.lock);
  while (!s.cancelled && g_get_monotonic_time () < due)
    g_cond_wait_until (&s.cond, &s.lock, due);
  cancelled = s.cancelled;
  g_mutex_unlock (&s.lock);
  if (id)
    bro_cancellation_token_disconnect (cancel, id);
  g_mutex_clear (&s.lock);
  g_cond_clear (&s.cond);
  if (cancelled || (cancel && bro_cancellation_token_is_cancelled (cancel)))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return FALSE;
    }
  return TRUE;
}

BroTimerHandle *
bro_system_clock_timer (BroSystemClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data,
                        GDestroyNotify destroy)
{
  TimerState *t = g_new0 (TimerState, 1);
  BroSystemTimer *handle = g_object_new (BRO_TYPE_SYSTEM_TIMER, NULL);
  g_atomic_ref_count_init (&t->refs);
  g_mutex_init (&t->lock);
  g_cond_init (&t->cond);
  t->schedule = *schedule;
  t->handler = handler;
  t->data = data;
  t->destroy = destroy;
  handle->state = t;
  g_thread_unref (g_thread_new ("bro-timer", timer_thread, timer_state_ref (t)));
  return BRO_TIMER_HANDLE (handle);
}

static BroInstant
now_vfunc (BroClock *self)
{
  return bro_system_clock_now (BRO_SYSTEM_CLOCK (self));
}

static BroDuration
monotonic_vfunc (BroClock *self)
{
  return bro_system_clock_monotonic (BRO_SYSTEM_CLOCK (self));
}

static gboolean
sleep_vfunc (BroClock *self, BroDuration duration, BroCancellationToken *cancel, BroBroError **error)
{
  return bro_system_clock_sleep (BRO_SYSTEM_CLOCK (self), duration, cancel, error);
}

static BroTimerHandle *
timer_vfunc (BroClock *self, const BroTimerSchedule *schedule, BroTimerFunc handler, gpointer data, GDestroyNotify destroy)
{
  return bro_system_clock_timer (BRO_SYSTEM_CLOCK (self), schedule, handler, data, destroy);
}

static void
bro_system_clock_class_init (BroSystemClockClass *klass)
{
}

static void
bro_system_clock_init (BroSystemClock *self)
{
}

static void
bro_system_clock_iface_init (BroClockInterface *iface)
{
  iface->now = now_vfunc;
  iface->monotonic = monotonic_vfunc;
  iface->sleep = sleep_vfunc;
  iface->timer = timer_vfunc;
}

BroSystemClock *
bro_system_clock_new (void)
{
  return g_object_new (BRO_TYPE_SYSTEM_CLOCK, NULL);
}
