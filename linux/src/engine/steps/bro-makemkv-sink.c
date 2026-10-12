/* bro-makemkv-sink.c */
#include "bro-makemkv-sink.h"
#include "bro-message-catalog.h"
#include "bro-steps-private.h"

struct _BroMakemkvSink {
  GObject parent;
  GMutex lock;
  BroProgressReporter *progress;
  BroJobLogger *log;
  BroBroMessage *operation;
  char *current;
  char *total;
};

static void sink_iface_init (BroRunSinkInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroMakemkvSink, bro_makemkv_sink, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_RUN_SINK, sink_iface_init))

static void
log_message (BroMakemkvSink *self, BroMessageCode code, BroJsonValue *params, BroSeverity severity)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, params, severity);
  bro_job_logger_log (self->log, m);
}

static void
sink_event (BroRunSink *sink, const BroRobotEvent *e)
{
  BroMakemkvSink *self = BRO_MAKEMKV_SINK (sink);
  g_mutex_lock (&self->lock);
  switch (e->kind) {
  case BRO_ROBOT_EVENT_MESSAGE: {
    BroMessageKind kind = bro_message_catalog_severity (e->message);
    if (kind != BRO_MESSAGE_KIND_DEBUG)
      log_message (self, BRO_MSG_MAKEMKV_MESSAGE, _bro_param ("text", bro_json_value_new_string (e->message->text)),
                   kind == BRO_MESSAGE_KIND_ERROR     ? BRO_SEVERITY_ERROR
                   : kind == BRO_MESSAGE_KIND_WARNING ? BRO_SEVERITY_WARNING
                                                      : BRO_SEVERITY_INFO);
    break;
  }
  case BRO_ROBOT_EVENT_PROGRESS_TOTAL:
    if (g_strcmp0 (e->name, self->total) != 0) {
      g_free (self->total);
      self->total = g_strdup (e->name);
      log_message (self, BRO_MSG_MAKEMKV_TITLE_PROGRESS, _bro_param ("name", bro_json_value_new_string (e->name)), BRO_SEVERITY_INFO);
    }
    break;
  case BRO_ROBOT_EVENT_PROGRESS_CURRENT:
    g_free (self->current);
    self->current = g_strdup (e->name);
    break;
  case BRO_ROBOT_EVENT_PROGRESS_VALUE:
    if (e->max > 0)
      bro_progress_reporter_report (self->progress, CLAMP ((double) e->total / e->max, 0.0, 1.0), self->operation, self->current);
    break;
  default:
    break;
  }
  g_mutex_unlock (&self->lock);
}

static void
sink_iface_init (BroRunSinkInterface *iface)
{
  iface->event = sink_event;
}

static void
bro_makemkv_sink_finalize (GObject *object)
{
  BroMakemkvSink *self = BRO_MAKEMKV_SINK (object);
  g_clear_object (&self->progress);
  g_clear_object (&self->log);
  bro_bro_message_free (self->operation);
  g_free (self->current);
  g_free (self->total);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_makemkv_sink_parent_class)->finalize (object);
}

static void
bro_makemkv_sink_class_init (BroMakemkvSinkClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_makemkv_sink_finalize;
}

static void
bro_makemkv_sink_init (BroMakemkvSink *self)
{
  g_mutex_init (&self->lock);
}

BroMakemkvSink *
bro_makemkv_sink_new (BroProgressReporter *progress, BroJobLogger *log, const BroBroMessage *operation)
{
  BroMakemkvSink *self = g_object_new (BRO_TYPE_MAKEMKV_SINK, NULL);
  self->progress = g_object_ref (progress);
  self->log = g_object_ref (log);
  self->operation = bro_bro_message_copy (operation);
  return self;
}
