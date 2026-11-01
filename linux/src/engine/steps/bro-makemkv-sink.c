/* bro-makemkv-sink.c */
#include "bro-makemkv-sink.h"
#include "bro-message-catalog.h"
#include "bro-seal-step.h"
#include "bro-steps-private.h"

#include <string.h>

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

void
bro_makemkv_sink_keep_debug_log (const BroStepContext *context, const BroRunOutcome *outcome, const char *command)
{
  BroFileSystem *fs = context->fs;
  const char *path = outcome->debug_log;
  g_autoptr (GBytes) text = NULL;
  g_autoptr (GBytes) before = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (BroBroError) ignored = NULL;
  g_autofree char *file = NULL;
  GByteArray *all;
  gsize n;
  const guint8 *t;
  if (!path || !*path || !bro_file_system_exists (fs, path))
    return;
  if (!(text = bro_file_system_read (fs, path, &ignored)) || g_bytes_get_size (text) == 0)
    return; /* a file that can't be read is left out (the run's result stands) */
  file = g_strconcat (context->job_directory, "/" BRO_SEAL_STEP_DEBUG_TRANSCRIPT, NULL);
  if (bro_file_system_exists (fs, file) && !(before = bro_file_system_read (fs, file, &ignored)))
    return;
  if (!bro_file_system_exists (fs, context->job_directory) && !bro_file_system_create_directory (fs, context->job_directory, FALSE, &ignored))
    return;
  all = g_byte_array_new ();
  if (before)
    g_byte_array_append (all, g_bytes_get_data (before, NULL), g_bytes_get_size (before));
  {
    g_autofree char *header = g_strconcat ("==== ", path, " after $ makemkvcon ", command, "\n", NULL);
    g_byte_array_append (all, (const guint8 *) header, strlen (header));
  }
  t = g_bytes_get_data (text, &n);
  g_byte_array_append (all, t, n);
  if (t[n - 1] != '\n')
    g_byte_array_append (all, (const guint8 *) "\n", 1);
  bytes = g_byte_array_free_to_bytes (all);
  bro_file_system_write_atomically (fs, file, bytes, 0600, &ignored); /* 0600, as the job's other files */
}
