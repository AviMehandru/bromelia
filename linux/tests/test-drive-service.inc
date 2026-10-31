/* test-drive-service.inc: shared/fixtures/engine/drive-service.cases.json, on a real executor, bus, problem service
 * and engine clock with fake ports. Included after test-services.inc (failure_of, problems_json). */

#include "bro-drive-service.h"
#include "bro-drive-record.h"
#include "bro-makemkv-drive.h"
#include "bro-robot.h"

#define TEST_TYPE_DRIVE_FAKES (test_drive_fakes_get_type ())
G_DECLARE_FINAL_TYPE (TestDriveFakes, test_drive_fakes, TEST, DRIVE_FAKES, GObject)

struct _TestDriveFakes {
  GObject parent_instance;
  GMutex lock;
  GPtrArray *calls;      /* char * */
  BroJsonValue *makemkv; /* DRV lines */
  GPtrArray *os;         /* BroOsDrive * */
  BroJsonValue *scan_fails, *eject_fails;
  BroDeviceSinkFunc sink;
  gpointer sink_data;
  GHashTable *stored; /* id → BroDriveRecord * */
  BroConfig *config;
};

static void drive_fakes_makemkv_iface (BroMakemkvInterface *iface);
static void drive_fakes_monitor_iface (BroDeviceMonitorInterface *iface);
static void drive_fakes_control_iface (BroDriveControlInterface *iface);
static void drive_fakes_repository_iface (BroDriveRepositoryInterface *iface);
static void drive_fakes_config_iface (BroConfigSourceInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (TestDriveFakes, test_drive_fakes, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_MAKEMKV, drive_fakes_makemkv_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DEVICE_MONITOR, drive_fakes_monitor_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_CONTROL, drive_fakes_control_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_REPOSITORY, drive_fakes_repository_iface)
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_CONFIG_SOURCE, drive_fakes_config_iface))

static void
drive_call (TestDriveFakes *f, char *call)
{
  g_mutex_lock (&f->lock);
  g_ptr_array_add (f->calls, call);
  g_mutex_unlock (&f->lock);
}

static BroDriveRecord *
drive_record_copy (const BroDriveRecord *r)
{
  BroDriveRecord *c = bro_drive_record_new ();
  *c = *r;
  c->id = g_strdup (r->id);
  c->identification = g_strdup (r->identification);
  c->model = g_strdup (r->model);
  c->last_device = g_strdup (r->last_device);
  c->config_id = g_strdup (r->config_id);
  return c;
}

static GPtrArray *
df_scan (BroMakemkv *m, BroCancellationToken *cancel, BroBroError **error)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (m);
  GPtrArray *drives;
  drive_call (f, g_strdup ("scan"));
  g_mutex_lock (&f->lock);
  if (f->scan_fails)
    {
      *error = failure_of (f->scan_fails);
      g_mutex_unlock (&f->lock);
      return NULL;
    }
  drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_makemkv_drive_free);
  for (guint i = 0; i < bro_json_value_length (f->makemkv); i++)
    {
      g_autoptr (BroRobotEvent) e = bro_robot_parse_line (bro_json_value_get_string (bro_json_value_at (f->makemkv, i), ""));
      g_ptr_array_add (drives, bro_makemkv_drive_from (e));
    }
  g_mutex_unlock (&f->lock);
  return drives;
}

static void
drive_fakes_makemkv_iface (BroMakemkvInterface *iface)
{
  iface->scan_drives = df_scan;
}

static void
df_start (BroDeviceMonitor *m, BroDeviceSinkFunc sink, gpointer data, GDestroyNotify destroy)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (m);
  drive_call (f, g_strdup ("monitor.start"));
  g_mutex_lock (&f->lock);
  f->sink = sink;
  f->sink_data = data;
  g_mutex_unlock (&f->lock);
}

static void df_stop (BroDeviceMonitor *m) {}

static GPtrArray *
df_current (BroDeviceMonitor *m)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (m);
  GPtrArray *os = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_free);
  g_mutex_lock (&f->lock);
  for (guint i = 0; i < f->os->len; i++)
    g_ptr_array_add (os, bro_os_drive_copy (f->os->pdata[i]));
  g_mutex_unlock (&f->lock);
  return os;
}

static void
drive_fakes_monitor_iface (BroDeviceMonitorInterface *iface)
{
  iface->start = df_start;
  iface->stop = df_stop;
  iface->current_drives = df_current;
}

static gboolean
df_eject (BroDriveControl *c, const char *device, BroBroError **error)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (c);
  drive_call (f, g_strdup_printf ("eject %s", device));
  if (f->eject_fails)
    {
      *error = failure_of (f->eject_fails);
      return FALSE;
    }
  return TRUE;
}

static gboolean
df_close (BroDriveControl *c, const char *device, BroBroError **error)
{
  drive_call (TEST_DRIVE_FAKES (c), g_strdup_printf ("closeTray %s", device));
  return TRUE;
}

static BroDiscContent
df_probe (BroDriveControl *c, const char *device)
{
  drive_call (TEST_DRIVE_FAKES (c), g_strdup_printf ("probe %s", device));
  return BRO_DISC_CONTENT_VIDEO;
}

static void
drive_fakes_control_iface (BroDriveControlInterface *iface)
{
  iface->eject = df_eject;
  iface->close_tray = df_close;
  iface->probe_content = df_probe;
}

static gboolean
df_upsert (BroDriveRepository *r, const BroDriveRecord *drive, BroBroError **error)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (r);
  g_mutex_lock (&f->lock);
  g_hash_table_replace (f->stored, g_strdup (drive->id), drive_record_copy (drive));
  g_mutex_unlock (&f->lock);
  return TRUE;
}

static GPtrArray *
df_all (BroDriveRepository *r, BroBroError **error)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (r);
  GPtrArray *rows = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_record_free);
  GHashTableIter it;
  gpointer v;
  g_mutex_lock (&f->lock);
  g_hash_table_iter_init (&it, f->stored);
  while (g_hash_table_iter_next (&it, NULL, &v))
    g_ptr_array_add (rows, drive_record_copy (v));
  g_mutex_unlock (&f->lock);
  return rows;
}

static void
drive_fakes_repository_iface (BroDriveRepositoryInterface *iface)
{
  iface->upsert = df_upsert;
  iface->all = df_all;
}

static BroConfig *
df_config (BroConfigSource *s)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (s);
  BroConfig *c;
  g_mutex_lock (&f->lock);
  c = bro_config_new (bro_json_value_ref (f->config->document), g_ptr_array_new ());
  g_mutex_unlock (&f->lock);
  return c;
}

static void
drive_fakes_config_iface (BroConfigSourceInterface *iface)
{
  iface->current = df_config;
}

static void
test_drive_fakes_finalize (GObject *o)
{
  TestDriveFakes *f = TEST_DRIVE_FAKES (o);
  g_ptr_array_unref (f->calls);
  g_clear_pointer (&f->makemkv, bro_json_value_unref);
  g_ptr_array_unref (f->os);
  g_clear_pointer (&f->scan_fails, bro_json_value_unref);
  g_clear_pointer (&f->eject_fails, bro_json_value_unref);
  g_hash_table_unref (f->stored);
  g_clear_pointer (&f->config, bro_config_free);
  g_mutex_clear (&f->lock);
  G_OBJECT_CLASS (test_drive_fakes_parent_class)->finalize (o);
}

static void test_drive_fakes_class_init (TestDriveFakesClass *k) { G_OBJECT_CLASS (k)->finalize = test_drive_fakes_finalize; }

static void
test_drive_fakes_init (TestDriveFakes *f)
{
  g_mutex_init (&f->lock);
  f->calls = g_ptr_array_new_with_free_func (g_free);
  f->makemkv = bro_json_value_new_array ();
  f->os = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_free);
  f->stored = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_drive_record_free);
}

/* ---- the harness ---- */

static const char *drive_case_id;

typedef struct {
  World w;
  BroEventBus *bus;
  BroProblemService *problems;
  TestDriveFakes *fakes;
  BroDriveService *service;
  BroSubscription *subscription;
  GPtrArray *events; /* BroEngineEvent *, on the executor */
  GMutex lock;
  GCond cond;
  gboolean done, checked, ok;
  BroBroError *error;
} DriveHarness;

static void
drive_record_event (BroEngineEvent *event, gpointer data)
{
  DriveHarness *h = data;
  g_ptr_array_add (h->events, bro_engine_event_ref (event));
}

static void
drive_settle_check (gpointer data)
{
  DriveHarness *h = data;
  /* Posted alone (no barrier after it), so the executor's activity counts only the engine's own work. */
  gboolean ok = bro_engine_executor_activity (h->w.executor) == 0;
  g_mutex_lock (&h->lock);
  h->ok = ok;
  h->checked = TRUE;
  g_cond_broadcast (&h->cond);
  g_mutex_unlock (&h->lock);
}

static void
drive_settle (gpointer data)
{
  DriveHarness *h = data;
  gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  if (bro_engine_executor_is_current (h->w.executor))
    return;
  for (;;)
    {
      gboolean ok;
      g_mutex_lock (&h->lock);
      h->checked = FALSE;
      g_mutex_unlock (&h->lock);
      if (!bro_engine_executor_post (h->w.executor, drive_settle_check, h, NULL))
        return;
      g_mutex_lock (&h->lock);
      while (!h->checked)
        g_cond_wait (&h->cond, &h->lock);
      ok = h->ok;
      g_mutex_unlock (&h->lock);
      if (ok)
        return;
      if (g_get_monotonic_time () > until)
        g_error ("%s: the service didn't settle", drive_case_id);
      g_usleep (1000);
    }
}

static void
drive_done (const BroBroError *error, gpointer data)
{
  DriveHarness *h = data;
  g_mutex_lock (&h->lock);
  h->error = error ? bro_bro_error_copy (error) : NULL;
  h->done = TRUE;
  g_cond_broadcast (&h->cond);
  g_mutex_unlock (&h->lock);
}

typedef struct {
  DriveHarness *h;
  const char *op;
  BroJsonValue *arg;
} DriveOp;

static void
drive_op_action (gpointer data)
{
  DriveOp *o = data;
  DriveHarness *h = o->h;
  const char *id = bro_json_value_get_string (o->arg, NULL);
  if (strcmp (o->op, "start") == 0)
    bro_drive_service_start (h->service, drive_done, h, NULL);
  else if (strcmp (o->op, "rescan") == 0)
    bro_drive_service_rescan (h->service, drive_done, h, NULL);
  else if (strcmp (o->op, "eject") == 0)
    bro_drive_service_eject (h->service, id, drive_done, h, NULL);
  else if (strcmp (o->op, "closeTray") == 0)
    bro_drive_service_close_tray (h->service, id, drive_done, h, NULL);
  else if (strcmp (o->op, "closeAll") == 0)
    bro_drive_service_close_all (h->service, drive_done, h, NULL);
}

/* Runs a command on the executor and waits for it; its failure is the result (else NULL). */
static BroJsonValue *
drive_command (DriveHarness *h, const char *op, BroJsonValue *arg)
{
  DriveOp o = { h, op, arg };
  gint64 until = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  BroJsonValue *result = NULL;
  g_mutex_lock (&h->lock);
  h->done = FALSE;
  g_mutex_unlock (&h->lock);
  bro_engine_executor_post (h->w.executor, drive_op_action, &o, NULL);
  g_mutex_lock (&h->lock);
  while (!h->done)
    if (!g_cond_wait_until (&h->cond, &h->lock, until))
      g_error ("%s: %s didn't finish", drive_case_id, op);
  if (h->error)
    {
      BroJsonValue *e = bro_json_value_new_object ();
      bro_json_value_set (e, "code", bro_json_value_new_string (h->error->code));
      bro_json_value_set (e, "params", bro_json_value_ref (h->error->params));
      result = bro_json_value_new_object ();
      bro_json_value_set (result, "error", e);
      g_clear_pointer (&h->error, bro_bro_error_free);
    }
  g_mutex_unlock (&h->lock);
  drive_settle (h);
  return result;
}

typedef struct {
  DriveHarness *h;
  const char *what;
  BroJsonValue *arg;
  BroJsonValue *result;
} DriveOnExecutor;

static void
drive_on_executor_action (gpointer data)
{
  DriveOnExecutor *x = data;
  DriveHarness *h = x->h;
  if (strcmp (x->what, "lease") == 0)
    {
      BroJsonValue *json = bro_json_value_new_object (), *jobs = bro_json_value_new_object ();
      bro_json_value_set (json, "drives", bro_json_value_ref (bro_json_value_member (x->arg, "drives")));
      bro_json_value_set (json, "acquisitions", bro_json_value_ref (bro_json_value_member (x->arg, "acquisitions")));
      bro_json_value_set (jobs, "acquisition", bro_json_value_new_integer (0));
      bro_json_value_set (jobs, "processing", bro_json_value_new_integer (0));
      bro_json_value_set (jobs, "maintenance", bro_json_value_new_integer (0));
      bro_json_value_set (json, "jobs", jobs);
      g_autoptr (BroEngineEvent) e = bro_event_bus_publish (h->bus, BRO_ENGINE_EVENT_TYPE_LEASE_CHANGED, json);
      bro_json_value_unref (json);
    }
  else if (strcmp (x->what, "config") == 0)
    {
      g_autoptr (GBytes) bytes = bro_json_value_encode_canonical (x->arg);
      gsize size;
      const char *text = g_bytes_get_data (bytes, &size);
      BroConfig *config = bro_config_codec_decode (text, (gssize) size, NULL);
      g_mutex_lock (&h->fakes->lock);
      bro_config_free (h->fakes->config);
      h->fakes->config = config;
      g_mutex_unlock (&h->fakes->lock);
      BroJsonValue *data = bro_json_value_new_object ();
      bro_json_value_set (data, "version", bro_json_value_new_integer (2));
      g_autoptr (BroEngineEvent) e = bro_event_bus_publish (h->bus, BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED, data);
      bro_json_value_unref (data);
    }
  else if (strcmp (x->what, "drives") == 0)
    {
      g_autoptr (GPtrArray) drives = bro_drive_service_drives (h->service);
      x->result = bro_json_value_new_array ();
      for (guint i = 0; i < drives->len; i++)
        bro_json_value_append (x->result, bro_drive_dto_json (drives->pdata[i]));
    }
  else if (strcmp (x->what, "subscribe") == 0)
    {
      static const BroEngineEventType types[] = { BRO_ENGINE_EVENT_TYPE_DRIVE_CHANGED, BRO_ENGINE_EVENT_TYPE_DRIVE_REMOVED,
                                                  BRO_ENGINE_EVENT_TYPE_MEDIUM_ARRIVED, BRO_ENGINE_EVENT_TYPE_MEDIUM_REMOVED,
                                                  BRO_ENGINE_EVENT_TYPE_PROBLEM_RAISED, BRO_ENGINE_EVENT_TYPE_PROBLEM_CLEARED };
      h->subscription = bro_event_bus_subscribe (h->bus, types, G_N_ELEMENTS (types), drive_record_event, h, NULL);
    }
  else if (strcmp (x->what, "stop") == 0)
    {
      bro_drive_service_stop (h->service);
      bro_subscription_cancel (h->subscription);
    }
  else if (strcmp (x->what, "problems") == 0)
    x->result = problems_json (h->problems);
  else if (strcmp (x->what, "events") == 0)
    x->result = events_json (h->events);
}

static BroJsonValue *
drive_on_executor (DriveHarness *h, const char *what, BroJsonValue *arg)
{
  DriveOnExecutor x = { h, what, arg, NULL };
  bro_engine_executor_post (h->w.executor, drive_on_executor_action, &x, NULL);
  bro_engine_executor_barrier (h->w.executor);
  return x.result;
}

static BroOsDrive *
drive_os_of (BroJsonValue *o)
{
  return bro_os_drive_new (bro_json_value_get_string (bro_json_value_member (o, "device"), ""),
                           bro_json_value_get_string (bro_json_value_member (o, "identification"), ""),
                           bro_json_value_get_string (bro_json_value_member (o, "mountPath"), NULL));
}

static void
drive_set_os (TestDriveFakes *f, BroJsonValue *list)
{
  g_ptr_array_set_size (f->os, 0);
  for (guint i = 0; i < bro_json_value_length (list); i++)
    g_ptr_array_add (f->os, drive_os_of (bro_json_value_at (list, i)));
}

static BroJsonValue *
drive_row_json (const BroDriveRecord *r)
{
  BroJsonValue *o = bro_json_value_new_object ();
  g_autofree char *first = bro_instant_format (r->first_seen_at);
  g_autofree char *last = bro_instant_format (r->last_seen_at);
  bro_json_value_set (o, "id", bro_json_value_new_string (r->id));
  bro_json_value_set (o, "identification", bro_json_value_new_string (r->identification));
  bro_json_value_set (o, "model", bro_json_value_new_string (r->model));
  bro_json_value_set (o, "lastDevice", bro_json_value_new_string (r->last_device));
  bro_json_value_set (o, "configId", r->config_id ? bro_json_value_new_string (r->config_id) : bro_json_value_new_null ());
  bro_json_value_set (o, "firstSeenAt", bro_json_value_new_string (first));
  bro_json_value_set (o, "lastSeenAt", bro_json_value_new_string (last));
  return o;
}

static BroJsonValue *
drive_location_json (BroDriveLocation *l)
{
  BroJsonValue *o;
  if (!l)
    return bro_json_value_new_null ();
  o = bro_json_value_new_object ();
  bro_json_value_set (o, "makemkvIndex", l->has_makemkv_index ? bro_json_value_new_integer (l->makemkv_index) : bro_json_value_new_null ());
  bro_json_value_set (o, "device", bro_json_value_new_string (l->device));
  bro_json_value_set (o, "configId", l->config_id ? bro_json_value_new_string (l->config_id) : bro_json_value_new_null ());
  bro_json_value_set (o, "generation", l->has_generation ? bro_json_value_new_integer (l->generation) : bro_json_value_new_null ());
  bro_json_value_set (o, "label", bro_json_value_new_string (l->label));
  bro_json_value_set (o, "flags", l->has_flags ? bro_json_value_new_integer (l->flags.raw) : bro_json_value_new_null ());
  return o;
}

static gint
drive_row_order (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDriveRecord *const *) a)->id, (*(BroDriveRecord *const *) b)->id);
}

static BroDeviceEvent *
drive_event_of (BroJsonValue *e)
{
  static const char *const kinds[] = { "driveAppeared", "driveVanished", "mediaArrived", "mediaRemoved", "trayOpened", "mounted", "unmounted" };
  BroDeviceEvent *d = bro_device_event_new ();
  const char *type = bro_json_value_get_string (bro_json_value_member (e, "type"), "");
  for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
    if (strcmp (kinds[i], type) == 0)
      d->kind = (BroDeviceEventKind) i;
  d->device = g_strdup (bro_json_value_get_string (bro_json_value_member (e, "device"), ""));
  d->path = g_strdup (bro_json_value_get_string (bro_json_value_member (e, "path"), NULL));
  if (d->kind == BRO_DEVICE_EVENT_DRIVE_APPEARED)
    d->drive = bro_os_drive_new (d->device, "", NULL);
  return d;
}

static gboolean
drive_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  DriveHarness h = { 0 };
  BroJsonValue *v, *ops = bro_json_value_member (given, "ops");
  g_autoptr (BroJsonValue) results = bro_json_value_new_array ();
  drive_case_id = id;
  world_init (&h.w);
  g_mutex_init (&h.lock);
  g_cond_init (&h.cond);
  h.events = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_engine_event_unref);
  h.bus = bro_event_bus_new (BRO_CLOCK (h.w.clock), 1, 1024, 1024);
  h.problems = bro_problem_service_new (h.bus, BRO_CLOCK (h.w.clock));
  h.fakes = g_object_new (TEST_TYPE_DRIVE_FAKES, NULL);
  if ((v = bro_json_value_member (given, "makemkv")))
    {
      bro_json_value_unref (h.fakes->makemkv);
      h.fakes->makemkv = bro_json_value_ref (v);
    }
  drive_set_os (h.fakes, bro_json_value_member (given, "os"));
  if ((v = bro_json_value_member (given, "ejectFails")))
    h.fakes->eject_fails = bro_json_value_ref (v);
  {
    v = bro_json_value_member (given, "config");
    g_autoptr (GBytes) bytes = v ? bro_json_value_encode_canonical (v) : g_bytes_new_static ("{\"version\": 3}", 14);
    gsize size;
    const char *text = g_bytes_get_data (bytes, &size);
    h.fakes->config = bro_config_codec_decode (text, (gssize) size, NULL);
  }
  v = bro_json_value_member (given, "stored");
  for (guint i = 0; i < bro_json_value_length (v); i++)
    {
      BroJsonValue *r = bro_json_value_at (v, i);
      BroDriveRecord *row = bro_drive_record_new ();
      row->id = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "id"), ""));
      row->identification = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "identification"), ""));
      row->model = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "model"), ""));
      row->last_device = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "lastDevice"), ""));
      row->config_id = g_strdup (bro_json_value_get_string (bro_json_value_member (r, "configId"), NULL));
      bro_instant_parse (bro_json_value_get_string (bro_json_value_member (r, "firstSeenAt"), ""), &row->first_seen_at);
      bro_instant_parse (bro_json_value_get_string (bro_json_value_member (r, "lastSeenAt"), ""), &row->last_seen_at);
      g_hash_table_replace (h.fakes->stored, g_strdup (row->id), row);
    }
  h.service = bro_drive_service_new (h.w.executor, h.bus, h.problems, BRO_CONFIG_SOURCE (h.fakes), h.w.engine_clock, BRO_DEVICE_MONITOR (h.fakes),
                                     BRO_DRIVE_CONTROL (h.fakes), BRO_MAKEMKV (h.fakes), BRO_DRIVE_REPOSITORY (h.fakes));
  drive_on_executor (&h, "subscribe", NULL);

  for (guint i = 0; i < bro_json_value_length (ops); i++)
    {
      BroJsonValue *op = bro_json_value_at (ops, i);
      const char *name = op->kind == BRO_JSON_VALUE_STRING ? op->string : op->keys->pdata[0];
      BroJsonValue *arg = op->kind == BRO_JSON_VALUE_STRING ? NULL : op->items->pdata[0];
      BroJsonValue *result = NULL;
      if (strcmp (name, "start") == 0 || strcmp (name, "rescan") == 0 || strcmp (name, "eject") == 0 || strcmp (name, "closeTray") == 0
          || strcmp (name, "closeAll") == 0)
        result = drive_command (&h, name, arg);
      else if (strcmp (name, "set") == 0)
        {
          g_mutex_lock (&h.fakes->lock);
          if ((v = bro_json_value_member (arg, "makemkv")))
            {
              bro_json_value_unref (h.fakes->makemkv);
              h.fakes->makemkv = bro_json_value_ref (v);
            }
          if ((v = bro_json_value_member (arg, "os")))
            drive_set_os (h.fakes, v);
          if ((v = bro_json_value_member (arg, "scanFails")))
            {
              g_clear_pointer (&h.fakes->scan_fails, bro_json_value_unref);
              if (v->kind != BRO_JSON_VALUE_NULL)
                h.fakes->scan_fails = bro_json_value_ref (v);
            }
          g_mutex_unlock (&h.fakes->lock);
        }
      else if (strcmp (name, "device") == 0)
        {
          g_autoptr (BroDeviceEvent) e = drive_event_of (arg);
          h.fakes->sink (e, h.fakes->sink_data);
          drive_settle (&h);
        }
      else if (strcmp (name, "advance") == 0)
        {
          test_manual_clock_advance (h.w.clock, bro_json_value_get_integer (arg, 0) * 1000, drive_settle, &h);
          drive_settle (&h);
        }
      else if (strcmp (name, "lease") == 0 || strcmp (name, "config") == 0)
        {
          drive_on_executor (&h, name, arg);
          drive_settle (&h);
        }
      else if (strcmp (name, "drives") == 0)
        result = drive_on_executor (&h, "drives", NULL);
      else if (strcmp (name, "find") == 0)
        {
          g_autoptr (BroDriveLocation) l = bro_drive_lookup_find (BRO_DRIVE_LOOKUP (h.service), bro_json_value_get_string (arg, ""));
          result = drive_location_json (l);
        }
      else
        g_error ("%s: unknown op %s", id, name);
      bro_json_value_append (results, result ? result : bro_json_value_new_null ());
    }
  drive_on_executor (&h, "stop", NULL);
  drive_settle (&h);

  if ((v = bro_json_value_member (expect, "results")))
    bro_test_same_json (failures, id, "results", v, results);
  if ((v = bro_json_value_member (expect, "events")))
    {
      g_autoptr (BroJsonValue) got = drive_on_executor (&h, "events", NULL);
      bro_test_same_json (failures, id, "events", v, got);
    }
  if ((v = bro_json_value_member (expect, "calls")))
    {
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      for (guint i = 0; i < h.fakes->calls->len; i++)
        bro_json_value_append (got, bro_json_value_new_string (h.fakes->calls->pdata[i]));
      bro_test_same_json (failures, id, "calls", v, got);
    }
  if ((v = bro_json_value_member (expect, "problems")))
    {
      g_autoptr (BroJsonValue) got = drive_on_executor (&h, "problems", NULL);
      bro_test_same_json (failures, id, "problems", v, got);
    }
  if ((v = bro_json_value_member (expect, "stored")))
    {
      g_autoptr (GPtrArray) rows = g_hash_table_get_values_as_ptr_array (h.fakes->stored);
      g_autoptr (BroJsonValue) got = bro_json_value_new_array ();
      g_ptr_array_sort (rows, drive_row_order);
      for (guint i = 0; i < rows->len; i++)
        bro_json_value_append (got, drive_row_json (rows->pdata[i]));
      bro_test_same_json (failures, id, "stored", v, got);
    }

  bro_engine_executor_stop (h.w.executor);
  g_clear_object (&h.subscription);
  g_ptr_array_unref (h.events);
  g_object_unref (h.service);
  g_object_unref (h.problems);
  g_object_unref (h.bus);
  g_object_unref (h.fakes);
  g_mutex_clear (&h.lock);
  g_cond_clear (&h.cond);
  world_clear (&h.w);
  return TRUE;
}

static void
test_drive_service (void)
{
  bro_test_run_cases ("engine/drive-service.cases.json", drive_case);
}
