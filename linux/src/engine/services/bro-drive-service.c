/* bro-drive-service.c */
#include "bro-drive-service.h"

#include "bro-drive-entry.h"
#include "bro-drive-join.h"
#include "bro-drive-location.h"
#include "bro-joined-drive.h"
#include "bro-lease-snapshot.h"
#include "bro-message-code.h"
#include "bro-problem-scope.h"
#include <string.h>

#define TRAY_CHECK_AFTER_MS 4000
#define SCAN_AFTER_TRAY_S 5
#define SCAN_AFTER_EJECT_S 3

typedef struct {
  char *id;
  char *entry_id;           /* a configured entry no connected drive matches (listed only) */
  BroMakemkvDrive *makemkv; /* NULL: never scanned */
  char *os_device;          /* NULL: not paired with an OS drive */
  gboolean connected;
  BroDriveStatus state;
  BroMediumDto *medium; /* NULL: no disc */
  BroDriveDto *shown;   /* NULL: never published */
} Drive;

typedef struct {
  BroDriveDone done;
  gpointer data;
  GDestroyNotify destroy;
} Waiter;

typedef struct {
  char *drive;
  gint64 at_ms;
} TrayAsk;

struct _BroDriveService {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroProblemService *problems;
  BroConfigSource *config;
  BroEngineClock *clock;
  BroDeviceMonitor *monitor;
  BroDriveControl *control;
  BroMakemkv *makemkv;
  BroDriveRepository *repository;

  GPtrArray *drives;       /* Drive * */
  GHashTable *generations; /* id → gint64 * */
  GHashTable *stored;      /* id → BroDriveRecord * */
  GPtrArray *tray_asked;   /* TrayAsk * */
  BroLeaseSnapshot *leases;
  gboolean started, first_done, scanning, again, deferred;
  GHashTable *held_in_scan; /* ids a scan left as they were because a job held them: scanned again once given back */
  GPtrArray *waiting, *next; /* Waiter * */
  BroEngineTimer *poll;
  gint64 poll_seconds;
  GPtrArray *timers; /* the one-shot scans after a tray command, cancelled at stop */
  BroSubscription *leases_sub, *config_sub;

  GMutex lock;
  GHashTable *snapshot; /* id → BroDriveLocation *; under the lock */
};

static void drive_lookup_iface_init (BroDriveLookupInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroDriveService, bro_drive_service, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_DRIVE_LOOKUP, drive_lookup_iface_init))

static void
drive_free (Drive *d)
{
  g_free (d->id);
  g_free (d->entry_id);
  g_clear_pointer (&d->makemkv, bro_makemkv_drive_free);
  g_free (d->os_device);
  g_clear_pointer (&d->medium, bro_medium_dto_free);
  g_clear_pointer (&d->shown, bro_drive_dto_free);
  g_free (d);
}

static Drive *
drive_new (const char *id, const char *entry_id)
{
  Drive *d = g_new0 (Drive, 1);
  d->id = g_strdup (id);
  d->entry_id = g_strdup (entry_id);
  d->state = BRO_DRIVE_STATUS_UNAVAILABLE;
  return d;
}

static void
waiter_free (Waiter *w)
{
  if (w->destroy)
    w->destroy (w->data);
  g_free (w);
}

static void
tray_ask_free (TrayAsk *t)
{
  g_free (t->drive);
  g_free (t);
}

static void
bro_drive_service_finalize (GObject *object)
{
  BroDriveService *self = BRO_DRIVE_SERVICE (object);
  g_clear_object (&self->executor);
  g_clear_object (&self->bus);
  g_clear_object (&self->problems);
  g_clear_object (&self->config);
  g_clear_object (&self->clock);
  g_clear_object (&self->monitor);
  g_clear_object (&self->control);
  g_clear_object (&self->makemkv);
  g_clear_object (&self->repository);
  g_ptr_array_unref (self->drives);
  g_hash_table_unref (self->generations);
  g_hash_table_unref (self->stored);
  g_ptr_array_unref (self->tray_asked);
  bro_lease_snapshot_free (self->leases);
  g_hash_table_unref (self->held_in_scan);
  g_ptr_array_unref (self->waiting);
  g_ptr_array_unref (self->next);
  g_clear_object (&self->poll);
  g_ptr_array_unref (self->timers);
  g_clear_object (&self->leases_sub);
  g_clear_object (&self->config_sub);
  g_hash_table_unref (self->snapshot);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (bro_drive_service_parent_class)->finalize (object);
}

static void
bro_drive_service_class_init (BroDriveServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_drive_service_finalize;
}

static void
bro_drive_service_init (BroDriveService *self)
{
  self->drives = g_ptr_array_new_with_free_func ((GDestroyNotify) drive_free);
  self->generations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->stored = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_drive_record_free);
  self->tray_asked = g_ptr_array_new_with_free_func ((GDestroyNotify) tray_ask_free);
  self->leases = bro_lease_snapshot_new ();
  self->held_in_scan = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->waiting = g_ptr_array_new_with_free_func ((GDestroyNotify) waiter_free);
  self->next = g_ptr_array_new_with_free_func ((GDestroyNotify) waiter_free);
  self->poll_seconds = -1;
  self->timers = g_ptr_array_new_with_free_func (g_object_unref);
  self->snapshot = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_drive_location_free);
  g_mutex_init (&self->lock);
}

BroDriveService *
bro_drive_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroConfigSource *config, BroEngineClock *clock,
                       BroDeviceMonitor *monitor, BroDriveControl *control, BroMakemkv *makemkv, BroDriveRepository *repository)
{
  BroDriveService *self = g_object_new (BRO_TYPE_DRIVE_SERVICE, NULL);
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->problems = g_object_ref (problems);
  self->config = g_object_ref (config);
  self->clock = g_object_ref (clock);
  self->monitor = g_object_ref (monitor);
  self->control = g_object_ref (control);
  self->makemkv = g_object_ref (makemkv);
  self->repository = g_object_ref (repository);
  return self;
}

/* ---- helpers ---- */

/* MakeMKV's device, else the OS's (macOS: an empty drive has no device in MakeMKV's list). Borrowed. */
static const char *
device_of (const Drive *d)
{
  if (d->makemkv && d->makemkv->device[0])
    return d->makemkv->device;
  return d->os_device ? d->os_device : "";
}

static gboolean
busy_holder (BroDriveService *self, const char *id, BroId *job)
{
  for (guint i = 0; i < self->leases->drives->len; i++)
    {
      const BroDriveHolder *h = self->leases->drives->pdata[i];
      if (strcmp (h->drive, id) == 0)
        {
          if (job)
            *job = h->job;
          return TRUE;
        }
    }
  return FALSE;
}

/* The entry's id (NULL: none), for @entries (BroDriveEntry *). Borrowed from @entries or @d. */
static const char *
config_id (BroDriveService *self, GPtrArray *entries, const Drive *d)
{
  if (d->makemkv)
    {
      BroDriveEntry *e = bro_drive_join_entry_for (entries, d->makemkv);
      return e ? e->id : NULL;
    }
  if (d->entry_id)
    return d->entry_id;
  BroDriveRecord *row = g_hash_table_lookup (self->stored, d->id);
  for (guint i = 0; row && row->config_id && i < entries->len; i++)
    if (strcmp (((BroDriveEntry *) entries->pdata[i])->id, row->config_id) == 0)
      return row->config_id;
  return NULL;
}

static BroDriveEntry *
entry_by_id (GPtrArray *entries, const char *id)
{
  for (guint i = 0; id && i < entries->len; i++)
    if (strcmp (((BroDriveEntry *) entries->pdata[i])->id, id) == 0)
      return entries->pdata[i];
  return NULL;
}

static const char *
identification_of (BroDriveService *self, const Drive *d)
{
  if (d->makemkv)
    return d->makemkv->identification;
  BroDriveRecord *row = g_hash_table_lookup (self->stored, d->id);
  return row ? row->identification : "";
}

/* A new string. */
static char *
name_of (BroDriveService *self, GPtrArray *entries, const Drive *d)
{
  BroDriveEntry *e = entry_by_id (entries, config_id (self, entries, d));
  return e ? g_strdup (e->name) : bro_drive_join_short_model (identification_of (self, d));
}

static BroDriveDto *
dto_of (BroDriveService *self, const BroConfig *config, GPtrArray *entries, const Drive *d)
{
  BroDriveDto *dto = bro_drive_dto_new ();
  const char *cid = config_id (self, entries, d);
  BroDriveEntry *e = entry_by_id (entries, cid);
  g_autoptr (BroJsonValue) automation = bro_config_automation (config, cid);
  dto->id = g_strdup (d->id);
  dto->config_id = g_strdup (cid);
  dto->name = name_of (self, entries, d);
  dto->identification = g_strdup (identification_of (self, d));
  dto->device = g_strdup (d->connected ? device_of (d) : "");
  dto->has_makemkv_index = d->connected && d->makemkv;
  dto->makemkv_index = dto->has_makemkv_index ? d->makemkv->index : 0;
  dto->connected = d->connected;
  dto->state = d->connected ? d->state : BRO_DRIVE_STATUS_UNAVAILABLE;
  dto->medium = d->connected ? bro_medium_dto_copy (d->medium) : NULL;
  dto->busy = busy_holder (self, d->id, &dto->job_id);
  dto->has_job_id = dto->busy;
  dto->profile_id = g_strdup (e && e->profile ? e->profile
                                              : bro_json_value_get_string (bro_json_value_member (config->document, "defaultProfile"), "default"));
  dto->rip_on_insert = bro_json_value_get_bool (bro_json_value_member (automation, "ripOnInsert"), FALSE);
  return dto;
}

static BroDriveStatus
status_of (BroDriveState state)
{
  switch (state)
    {
    case BRO_DRIVE_STATE_EMPTY_CLOSED: return BRO_DRIVE_STATUS_EMPTY;
    case BRO_DRIVE_STATE_EMPTY_OPEN: return BRO_DRIVE_STATUS_TRAY_OPEN;
    case BRO_DRIVE_STATE_INSERTED: return BRO_DRIVE_STATUS_PRESENT;
    case BRO_DRIVE_STATE_LOADING: return BRO_DRIVE_STATUS_LOADING;
    case BRO_DRIVE_STATE_UNMOUNTING: return BRO_DRIVE_STATUS_UNMOUNTING;
    default: return BRO_DRIVE_STATUS_UNAVAILABLE;
    }
}

static Drive *
find_drive (BroDriveService *self, const char *id)
{
  for (guint i = 0; i < self->drives->len; i++)
    if (strcmp (((Drive *) self->drives->pdata[i])->id, id) == 0)
      return self->drives->pdata[i];
  return NULL;
}

static gboolean
same_device (const Drive *d, const char *device)
{
  return g_strcmp0 (d->os_device, device) == 0 || strcmp (device_of (d), device) == 0;
}

/* A disc that left: (drive, generation). */
typedef struct {
  const Drive *drive;
  gint64 generation;
} Removed;

static void
disconnect (Drive *d, GArray *removed)
{
  if (d->medium)
    {
      Removed r = { d, d->medium->generation };
      g_array_append_val (removed, r);
    }
  g_clear_pointer (&d->medium, bro_medium_dto_free);
  d->connected = FALSE;
  d->state = BRO_DRIVE_STATUS_UNAVAILABLE;
}

static void
publish_data (BroDriveService *self, BroEngineEventType type, BroJsonValue *data)
{
  g_autoptr (BroEngineEvent) e = bro_event_bus_publish (self->bus, type, data);
  bro_json_value_unref (data);
}

/* mediumRemoved, driveChanged and mediumArrived per drive, in the list's order; then driveRemoved for the drives
 * without an entry that are gone, and the snapshot. @removed (Removed) and @arrived (Drive *) may be NULL. */
static void
publish (BroDriveService *self, GArray *removed, GPtrArray *arrived)
{
  g_autoptr (BroConfig) config = bro_config_source_current (self->config);
  g_autoptr (GPtrArray) entries = bro_config_drives (config);
  GHashTable *snapshot = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_drive_location_free);
  for (guint i = 0; i < self->drives->len;)
    {
      Drive *d = self->drives->pdata[i];
      for (guint k = 0; removed && k < removed->len; k++)
        {
          Removed *r = &g_array_index (removed, Removed, k);
          if (r->drive != d)
            continue;
          BroJsonValue *data = bro_json_value_new_object ();
          bro_json_value_set (data, "driveId", bro_json_value_new_string (d->id));
          bro_json_value_set (data, "generation", bro_json_value_new_integer (r->generation));
          publish_data (self, BRO_ENGINE_EVENT_TYPE_MEDIUM_REMOVED, data);
        }
      if (!d->connected && !config_id (self, entries, d))
        {
          if (d->shown)
            {
              BroJsonValue *data = bro_json_value_new_object ();
              bro_json_value_set (data, "driveId", bro_json_value_new_string (d->id));
              publish_data (self, BRO_ENGINE_EVENT_TYPE_DRIVE_REMOVED, data);
            }
          g_ptr_array_remove_index (self->drives, i); /* frees it */
          continue;
        }
      BroDriveDto *now = dto_of (self, config, entries, d);
      g_autoptr (BroJsonValue) now_json = bro_drive_dto_json (now);
      g_autoptr (BroJsonValue) was_json = d->shown ? bro_drive_dto_json (d->shown) : NULL;
      if (!was_json || !bro_json_value_equal (was_json, now_json))
        {
          bro_drive_dto_free (d->shown);
          d->shown = now;
          BroJsonValue *data = bro_json_value_new_object ();
          bro_json_value_set (data, "drive", bro_json_value_ref (now_json));
          publish_data (self, BRO_ENGINE_EVENT_TYPE_DRIVE_CHANGED, data);
        }
      else
        bro_drive_dto_free (now);
      if (arrived && d->medium && g_ptr_array_find (arrived, d, NULL))
        {
          BroJsonValue *data = bro_json_value_new_object ();
          bro_json_value_set (data, "driveId", bro_json_value_new_string (d->id));
          bro_json_value_set (data, "generation", bro_json_value_new_integer (d->medium->generation));
          bro_json_value_set (data, "medium", bro_medium_dto_json (d->medium));
          publish_data (self, BRO_ENGINE_EVENT_TYPE_MEDIUM_ARRIVED, data);
        }
      if (d->connected)
        {
          BroDriveLocation *l = bro_drive_location_new (device_of (d), config_id (self, entries, d), d->medium ? d->medium->label : "");
          l->has_makemkv_index = d->makemkv != NULL;
          l->makemkv_index = d->makemkv ? d->makemkv->index : 0;
          l->has_generation = d->medium != NULL;
          l->generation = d->medium ? d->medium->generation : 0;
          l->has_flags = d->medium && d->makemkv;
          if (l->has_flags)
            l->flags = d->makemkv->flags;
          g_hash_table_insert (snapshot, g_strdup (d->id), l);
        }
      i++;
    }
  g_mutex_lock (&self->lock);
  g_hash_table_unref (self->snapshot);
  self->snapshot = snapshot;
  g_mutex_unlock (&self->lock);
}

/* A tray closed at least 4 s ago that is still open is reported; one that isn't open any more, cleared. */
static void
check_trays (BroDriveService *self, GPtrArray *entries, gint64 now_ms)
{
  for (guint i = 0; i < self->tray_asked->len;)
    {
      TrayAsk *t = self->tray_asked->pdata[i];
      if (now_ms - t->at_ms < TRAY_CHECK_AFTER_MS)
        {
          i++;
          continue;
        }
      Drive *open = find_drive (self, t->drive);
      if (open && open->connected && open->state == BRO_DRIVE_STATUS_TRAY_OPEN)
        {
          BroJsonValue *params = bro_json_value_new_object ();
          g_autofree char *name = name_of (self, entries, open);
          bro_json_value_set (params, "drive", bro_json_value_new_string (name));
          g_autoptr (BroBroMessage) m = bro_bro_message_new (BRO_MSG_DRIVE_TRAY_STILL_OPEN, params, BRO_SEVERITY_WARNING);
          g_autoptr (BroProblemScope) scope = bro_problem_scope_new (BRO_PROBLEM_SCOPE_KIND_DRIVE, open->id);
          g_autoptr (BroProblemDto) p = bro_problem_service_raise (self->problems, m, scope, NULL, 0);
        }
      g_ptr_array_remove_index (self->tray_asked, i);
    }
  for (guint i = 0; i < self->drives->len; i++)
    {
      Drive *d = self->drives->pdata[i];
      if (d->state != BRO_DRIVE_STATUS_TRAY_OPEN || !d->connected)
        {
          g_autoptr (BroProblemScope) scope = bro_problem_scope_new (BRO_PROBLEM_SCOPE_KIND_DRIVE, d->id);
          bro_problem_service_clear (self->problems, BRO_MSG_DRIVE_TRAY_STILL_OPEN, scope);
        }
    }
}

/* ---- scans ---- */

typedef struct {
  BroDriveService *self; /* a reference */
  GHashTable *known;     /* id → "label\nflags" of the discs known before the scan */
  GHashTable *busy;      /* ids a job holds */
  GPtrArray *makemkv;    /* BroMakemkvDrive * */
  GPtrArray *os;         /* BroOsDrive * */
  GHashTable *contents;  /* id → GINT_TO_POINTER (content + 1) */
  BroBroError *failure;
  GPtrArray *records;    /* BroDriveRecord * to store */
} Scan;

static void
scan_free (Scan *s)
{
  g_object_unref (s->self);
  g_hash_table_unref (s->known);
  g_hash_table_unref (s->busy);
  g_clear_pointer (&s->makemkv, g_ptr_array_unref);
  g_clear_pointer (&s->os, g_ptr_array_unref);
  g_hash_table_unref (s->contents);
  g_clear_pointer (&s->failure, bro_bro_error_free);
  g_clear_pointer (&s->records, g_ptr_array_unref);
  g_free (s);
}

static char *
disc_key (const char *label, int flags)
{
  return g_strdup_printf ("%s\n%d", label, flags);
}

static gboolean
scan_work (gpointer data, BroBroError **error)
{
  Scan *s = data;
  BroDriveService *self = s->self;
  s->makemkv = bro_makemkv_scan_drives (self->makemkv, NULL, &s->failure);
  if (!s->makemkv)
    return TRUE;
  s->os = bro_device_monitor_current_drives (self->monitor);
  /* What is on each disc that just arrived (a known disc, or one a job is using, isn't read again). */
  g_autoptr (GPtrArray) joined = bro_drive_join_join (s->makemkv, s->os);
  for (guint i = 0; i < joined->len; i++)
    {
      const BroJoinedDrive *j = joined->pdata[i];
      if (!j->makemkv || j->makemkv->state != BRO_DRIVE_STATE_INSERTED || g_hash_table_contains (s->busy, j->drive_id))
        continue;
      g_autofree char *key = disc_key (j->makemkv->label, j->makemkv->flags.raw);
      if (g_strcmp0 (g_hash_table_lookup (s->known, j->drive_id), key) == 0)
        continue;
      const char *device = j->makemkv->device[0] ? j->makemkv->device : j->os ? j->os->device : "";
      BroDiscContent content = device[0] ? bro_drive_control_probe_content (self->control, device) : BRO_DISC_CONTENT_UNKNOWN;
      g_hash_table_insert (s->contents, g_strdup (j->drive_id), GINT_TO_POINTER (content + 1));
    }
  return TRUE;
}

/* The scan's drives into the state, with the events; the rows to store into s->records. */
static void
apply (BroDriveService *self, Scan *s)
{
  g_autoptr (BroConfig) config = bro_config_source_current (self->config);
  g_autoptr (GPtrArray) entries = bro_config_drives (config);
  s->records = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_record_free);
  if (s->failure)
    {
      BroJsonValue *params = bro_json_value_new_object (), *reason = bro_json_value_new_object ();
      bro_json_value_set (reason, "code", bro_json_value_new_string (s->failure->code));
      if (s->failure->params && bro_json_value_length (s->failure->params) > 0)
        bro_json_value_set (reason, "params", bro_json_value_ref (s->failure->params));
      bro_json_value_set (params, "reason", reason);
      g_autoptr (BroBroMessage) m = bro_bro_message_new (BRO_MSG_MAKEMKV_DRIVE_SCAN_FAILED, params, BRO_SEVERITY_ERROR);
      g_autoptr (BroProblemDto) p = bro_problem_service_raise (self->problems, m, NULL, NULL, 0);
      return;
    }
  bro_problem_service_clear (self->problems, BRO_MSG_MAKEMKV_DRIVE_SCAN_FAILED, NULL);
  BroInstant now = bro_engine_clock_now (self->clock);
  gboolean first = !self->first_done;
  self->first_done = TRUE;
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) order = g_ptr_array_new ();
  g_autoptr (GArray) removed = g_array_new (FALSE, FALSE, sizeof (Removed));
  g_autoptr (GPtrArray) arrived = g_ptr_array_new ();
  g_autoptr (GPtrArray) joined = bro_drive_join_join (s->makemkv, s->os);
  for (guint i = 0; i < joined->len; i++)
    {
      const BroJoinedDrive *j = joined->pdata[i];
      const BroMakemkvDrive *m = j->makemkv;
      if (!m)
        continue; /* an OS drive MakeMKV doesn't list */
      Drive *drive = find_drive (self, j->drive_id);
      if (!drive)
        {
          drive = drive_new (j->drive_id, NULL);
          g_ptr_array_add (self->drives, drive);
        }
      g_hash_table_add (seen, drive->id);
      g_ptr_array_add (order, drive);
      if (g_hash_table_contains (s->busy, drive->id) && drive->connected)
        {
          g_hash_table_add (self->held_in_scan, g_strdup (drive->id));
          continue; /* a job holds it: MakeMKV can't see it */
        }
      gboolean was_connected = drive->connected;
      g_clear_pointer (&drive->makemkv, bro_makemkv_drive_free);
      drive->makemkv = bro_makemkv_drive_copy (m);
      if (j->os)
        {
          g_free (drive->os_device);
          drive->os_device = g_strdup (j->os->device);
        }
      drive->connected = TRUE;
      drive->state = status_of (m->state);
      BroMediumDto *old = drive->medium;
      if (m->state == BRO_DRIVE_STATE_INSERTED)
        {
          gboolean same = FALSE;
          if (old && strcmp (old->label, m->label) == 0)
            {
              g_autoptr (GPtrArray) names = bro_medium_dto_flag_names (m->flags);
              same = names->len == old->flags->len;
              for (guint k = 0; same && k < names->len; k++)
                same = strcmp (names->pdata[k], old->flags->pdata[k]) == 0;
            }
          if (same)
            {
              if (j->os && j->os->mount_path)
                {
                  g_free (old->mount_path);
                  old->mount_path = g_strdup (j->os->mount_path);
                }
            }
          else
            {
              if (old)
                {
                  Removed r = { drive, old->generation };
                  g_array_append_val (removed, r);
                  bro_medium_dto_free (old);
                }
              gint64 *gen = g_hash_table_lookup (self->generations, drive->id);
              if (!gen)
                {
                  gen = g_new0 (gint64, 1);
                  g_hash_table_insert (self->generations, g_strdup (drive->id), gen);
                }
              *gen += 1;
              gpointer content = g_hash_table_lookup (s->contents, drive->id);
              drive->medium = bro_medium_dto_new (*gen, m->label, m->flags,
                                                  content ? (BroDiscContent) (GPOINTER_TO_INT (content) - 1) : BRO_DISC_CONTENT_UNKNOWN,
                                                  j->os ? j->os->mount_path : NULL);
              if (!first)
                g_ptr_array_add (arrived, drive);
            }
        }
      else
        {
          if (old)
            {
              Removed r = { drive, old->generation };
              g_array_append_val (removed, r);
            }
          g_clear_pointer (&drive->medium, bro_medium_dto_free);
        }
      const char *cid = config_id (self, entries, drive);
      BroDriveRecord *row = g_hash_table_lookup (self->stored, drive->id);
      if (!was_connected || !row || g_strcmp0 (row->last_device, device_of (drive)) != 0 || g_strcmp0 (row->config_id, cid) != 0)
        {
          BroDriveRecord *record = bro_drive_record_new ();
          record->id = g_strdup (drive->id);
          record->identification = g_strdup (m->identification);
          record->model = bro_drive_join_short_model (m->identification);
          record->last_device = g_strdup (device_of (drive));
          record->config_id = g_strdup (cid);
          record->first_seen_at = row ? row->first_seen_at : now;
          record->last_seen_at = now;
          g_ptr_array_add (s->records, record);
          BroDriveRecord *kept = bro_drive_record_new ();
          *kept = *record;
          kept->id = g_strdup (record->id);
          kept->identification = g_strdup (record->identification);
          kept->model = g_strdup (record->model);
          kept->last_device = g_strdup (record->last_device);
          kept->config_id = g_strdup (record->config_id);
          g_hash_table_replace (self->stored, g_strdup (drive->id), kept);
        }
    }
  /* A connected drive left out of the scan stays as it was, unless the OS no longer lists its device. */
  for (guint i = 0; i < self->drives->len; i++)
    {
      Drive *d = self->drives->pdata[i];
      if (!d->connected || g_hash_table_contains (seen, d->id) || !d->os_device)
        continue;
      gboolean listed = FALSE;
      for (guint k = 0; k < s->os->len && !listed; k++)
        listed = strcmp (((BroOsDrive *) s->os->pdata[k])->device, d->os_device) == 0;
      if (!listed)
        disconnect (d, removed);
    }
  /* The drives in the scan's order, then the others (stable, by MakeMKV's index). */
  {
    GPtrArray *sorted = g_ptr_array_new_with_free_func ((GDestroyNotify) drive_free);
    g_autoptr (GPtrArray) rest = g_ptr_array_new ();
    for (guint i = 0; i < order->len; i++)
      g_ptr_array_add (sorted, order->pdata[i]);
    for (guint i = 0; i < self->drives->len; i++)
      if (!g_ptr_array_find (order, self->drives->pdata[i], NULL))
        g_ptr_array_add (rest, self->drives->pdata[i]);
    for (int index = 0; rest->len > 0; index++)
      for (guint i = 0; i < rest->len;)
        {
          Drive *d = rest->pdata[i];
          if ((d->makemkv ? d->makemkv->index : 0) <= index)
            {
              g_ptr_array_add (sorted, d);
              g_ptr_array_remove_index (rest, i);
            }
          else
            i++;
        }
    g_ptr_array_set_free_func (self->drives, NULL);
    g_ptr_array_unref (self->drives);
    self->drives = sorted;
  }
  publish (self, removed, arrived);
  check_trays (self, entries, now.unix_milliseconds);
}

static void run_scan (BroDriveService *self);

static void
finish_scan (BroDriveService *self)
{
  GPtrArray *done = self->waiting;
  self->waiting = self->next;
  self->next = g_ptr_array_new_with_free_func ((GDestroyNotify) waiter_free);
  for (guint i = 0; i < done->len; i++)
    {
      Waiter *w = done->pdata[i];
      if (w->done)
        w->done (NULL, w->data);
    }
  g_ptr_array_unref (done);
  self->scanning = FALSE;
  if (self->again)
    {
      self->again = FALSE;
      run_scan (self);
    }
}

static gboolean
store_work (gpointer data, BroBroError **error)
{
  Scan *s = data;
  for (guint i = 0; i < s->records->len; i++)
    if (!bro_drive_repository_upsert (s->self->repository, s->records->pdata[i], NULL))
      break; /* the next scan writes it again: the row still differs */
  return TRUE;
}

static void
store_done (gpointer data, const BroBroError *error)
{
  Scan *s = data;
  finish_scan (s->self);
}

static void
scan_done (gpointer data, const BroBroError *error)
{
  Scan *s = data;
  BroDriveService *self = s->self;
  apply (self, s);
  if (s->records->len > 0)
    {
      Scan *keep = g_new0 (Scan, 1);
      *keep = *s;
      /* The store's worker takes everything; what is left in @s is cleared. */
      keep->self = g_object_ref (self);
      keep->known = g_hash_table_ref (s->known);
      keep->busy = g_hash_table_ref (s->busy);
      keep->contents = g_hash_table_ref (s->contents);
      keep->makemkv = NULL;
      keep->os = NULL;
      keep->failure = NULL;
      keep->records = g_steal_pointer (&s->records);
      bro_engine_executor_worker (self->executor, store_work, store_done, keep, (GDestroyNotify) scan_free);
      return;
    }
  finish_scan (self);
}

static void
run_scan (BroDriveService *self)
{
  Scan *s = g_new0 (Scan, 1);
  self->scanning = TRUE;
  s->self = g_object_ref (self);
  s->known = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  s->busy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  s->contents = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < self->drives->len; i++)
    {
      Drive *d = self->drives->pdata[i];
      if (d->medium)
        g_hash_table_insert (s->known, g_strdup (d->id), disc_key (d->medium->label, d->makemkv ? d->makemkv->flags.raw : 0));
    }
  for (guint i = 0; i < self->leases->drives->len; i++)
    g_hash_table_add (s->busy, g_strdup (((BroDriveHolder *) self->leases->drives->pdata[i])->drive));
  bro_engine_executor_worker (self->executor, scan_work, scan_done, s, (GDestroyNotify) scan_free);
}

static void
waiter_now (gpointer data)
{
  Waiter *w = data;
  if (w->done)
    w->done (NULL, w->data);
}

/* Starts a scan, or one more after the scan that is out; a scan nobody asked for waits while an acquisition runs
 * (unless automation.pollWhileRipping) and runs when it ends. @done (may be NULL) runs when it is applied, or at once
 * (posted) when no scan runs. */
static void
scan (BroDriveService *self, gboolean asked, BroDriveDone done, gpointer data, GDestroyNotify destroy)
{
  Waiter *w = g_new0 (Waiter, 1);
  w->done = done;
  w->data = data;
  w->destroy = destroy;
  if (!self->started)
    {
      bro_engine_executor_post (self->executor, waiter_now, w, (GDestroyNotify) waiter_free);
      return;
    }
  if (!asked && self->leases->acquisitions > 0)
    {
      g_autoptr (BroConfig) config = bro_config_source_current (self->config);
      if (!bro_json_value_get_bool (bro_json_value_member (bro_json_value_member (config->document, "automation"), "pollWhileRipping"), FALSE))
        {
          self->deferred = TRUE;
          bro_engine_executor_post (self->executor, waiter_now, w, (GDestroyNotify) waiter_free);
          return;
        }
    }
  if (self->scanning)
    {
      self->again = TRUE;
      g_ptr_array_add (self->next, w);
      return;
    }
  g_ptr_array_add (self->waiting, w);
  run_scan (self);
}

static void
poll_tick (gpointer data)
{
  scan (data, FALSE, NULL, NULL, NULL);
}

static void
asked_tick (gpointer data)
{
  scan (data, TRUE, NULL, NULL, NULL);
}

static void
schedule_poll (BroDriveService *self)
{
  g_autoptr (BroConfig) config = bro_config_source_current (self->config);
  gint64 seconds = bro_json_value_get_integer (bro_json_value_member (bro_json_value_member (config->document, "automation"), "pollIntervalSeconds"), 10);
  if (seconds == self->poll_seconds)
    return;
  self->poll_seconds = seconds;
  if (self->poll)
    {
      bro_engine_timer_cancel (self->poll);
      g_clear_object (&self->poll);
    }
  if (seconds > 0)
    {
      BroTimerSchedule every = { BRO_TIMER_SCHEDULE_EVERY, { 0 }, { (double) seconds } };
      self->poll = bro_engine_clock_timer (self->clock, &every, poll_tick, self, NULL);
    }
}

static void
scan_in (BroDriveService *self, int seconds)
{
  BroInstant now = bro_engine_clock_now (self->clock);
  BroTimerSchedule at = { BRO_TIMER_SCHEDULE_AT, { now.unix_milliseconds + seconds * 1000 }, { 0 } };
  g_ptr_array_add (self->timers, bro_engine_clock_timer (self->clock, &at, asked_tick, self, NULL));
}

/* ---- events ---- */

static void
on_leases (BroEngineEvent *event, gpointer data)
{
  BroDriveService *self = data;
  BroLeaseSnapshot *leases = bro_lease_snapshot_parse (event->data);
  if (!leases)
    return;
  bro_lease_snapshot_free (self->leases);
  self->leases = leases;
  publish (self, NULL, NULL);
  gboolean given = FALSE;
  GHashTableIter it;
  gpointer id;
  g_hash_table_iter_init (&it, self->held_in_scan);
  while (g_hash_table_iter_next (&it, &id, NULL))
    {
      gboolean held = FALSE;
      for (guint i = 0; i < leases->drives->len && !held; i++)
        held = strcmp (((BroDriveHolder *) leases->drives->pdata[i])->drive, id) == 0;
      if (!held)
        {
          g_hash_table_iter_remove (&it);
          given = TRUE;
        }
    }
  if ((self->deferred && leases->acquisitions == 0) || given)
    {
      self->deferred = FALSE;
      scan (self, FALSE, NULL, NULL, NULL);
    }
}

static void
on_config (BroEngineEvent *event, gpointer data)
{
  BroDriveService *self = data;
  schedule_poll (self);
  publish (self, NULL, NULL);
}

static void
set_mount (BroDriveService *self, const char *device, const char *path)
{
  for (guint i = 0; i < self->drives->len; i++)
    {
      Drive *d = self->drives->pdata[i];
      if (d->connected && d->medium && same_device (d, device))
        {
          g_free (d->medium->mount_path);
          d->medium->mount_path = g_strdup (path);
        }
    }
  publish (self, NULL, NULL);
}

typedef struct {
  BroDriveService *self; /* a reference */
  BroDeviceEvent *event;
} Posted;

static void
posted_free (Posted *p)
{
  g_object_unref (p->self);
  bro_device_event_free (p->event);
  g_free (p);
}

static void
on_device_event (gpointer data)
{
  Posted *p = data;
  BroDriveService *self = p->self;
  const BroDeviceEvent *e = p->event;
  if (!self->started)
    return;
  switch (e->kind)
    {
    case BRO_DEVICE_EVENT_MOUNTED:
      set_mount (self, e->device, e->path);
      return;
    case BRO_DEVICE_EVENT_UNMOUNTED:
      set_mount (self, e->device, NULL);
      return;
    case BRO_DEVICE_EVENT_DRIVE_VANISHED:
      {
        g_autoptr (GArray) removed = g_array_new (FALSE, FALSE, sizeof (Removed));
        for (guint i = 0; i < self->drives->len; i++)
          {
            Drive *d = self->drives->pdata[i];
            if (d->connected && same_device (d, e->device))
              disconnect (d, removed);
          }
        publish (self, removed, NULL);
        break;
      }
    default:
      break;
    }
  scan (self, FALSE, NULL, NULL, NULL);
}

/* From the monitor's thread. */
static void
device_sink (const BroDeviceEvent *event, gpointer data)
{
  BroDriveService *self = data;
  Posted *p = g_new0 (Posted, 1);
  p->self = g_object_ref (self);
  p->event = bro_device_event_new ();
  p->event->kind = event->kind;
  p->event->drive = event->drive ? bro_os_drive_copy (event->drive) : NULL;
  p->event->device = g_strdup (event->device);
  p->event->path = g_strdup (event->path);
  bro_engine_executor_post (self->executor, on_device_event, p, (GDestroyNotify) posted_free);
}

/* ---- commands ---- */

typedef struct {
  BroDriveService *self; /* a reference */
  Waiter *waiter;
  GPtrArray *rows; /* start: BroDriveRecord * */
} Start;

static void
start_free (Start *s)
{
  g_object_unref (s->self);
  if (s->waiter)
    waiter_free (s->waiter);
  g_clear_pointer (&s->rows, g_ptr_array_unref);
  g_free (s);
}

static gboolean
start_work (gpointer data, BroBroError **error)
{
  Start *s = data;
  s->rows = bro_drive_repository_all (s->self->repository, error);
  return s->rows != NULL;
}

static void
start_done (gpointer data, const BroBroError *error)
{
  Start *s = data;
  BroDriveService *self = s->self;
  static const BroEngineEventType lease_types[] = { BRO_ENGINE_EVENT_TYPE_LEASE_CHANGED };
  static const BroEngineEventType config_types[] = { BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED };
  Waiter *w = g_steal_pointer (&s->waiter);
  if (error)
    {
      if (w->done)
        w->done (error, w->data);
      waiter_free (w);
      return;
    }
  for (guint i = 0; i < s->rows->len; i++)
    {
      BroDriveRecord *r = s->rows->pdata[i];
      g_hash_table_replace (self->stored, g_strdup (r->id), r);
      s->rows->pdata[i] = NULL;
    }
  self->leases_sub = bro_event_bus_subscribe (self->bus, lease_types, G_N_ELEMENTS (lease_types), on_leases, self, NULL);
  self->config_sub = bro_event_bus_subscribe (self->bus, config_types, G_N_ELEMENTS (config_types), on_config, self, NULL);
  self->started = TRUE;
  bro_device_monitor_start (self->monitor, device_sink, self, NULL);
  schedule_poll (self);
  scan (self, TRUE, w->done, w->data, w->destroy);
  w->destroy = NULL;
  waiter_free (w);
}

void
bro_drive_service_start (BroDriveService *self, BroDriveDone done, gpointer data, GDestroyNotify destroy)
{
  Start *s = g_new0 (Start, 1);
  s->self = g_object_ref (self);
  s->waiter = g_new0 (Waiter, 1);
  s->waiter->done = done;
  s->waiter->data = data;
  s->waiter->destroy = destroy;
  bro_engine_executor_worker (self->executor, start_work, start_done, s, (GDestroyNotify) start_free);
}

void
bro_drive_service_stop (BroDriveService *self)
{
  self->started = FALSE;
  bro_device_monitor_stop (self->monitor);
  if (self->poll)
    {
      bro_engine_timer_cancel (self->poll);
      g_clear_object (&self->poll);
    }
  for (guint i = 0; i < self->timers->len; i++)
    bro_engine_timer_cancel (self->timers->pdata[i]);
  g_ptr_array_set_size (self->timers, 0);
  if (self->leases_sub)
    {
      bro_subscription_cancel (self->leases_sub);
      g_clear_object (&self->leases_sub);
    }
  if (self->config_sub)
    {
      bro_subscription_cancel (self->config_sub);
      g_clear_object (&self->config_sub);
    }
}

GPtrArray *
bro_drive_service_drives (BroDriveService *self)
{
  g_autoptr (BroConfig) config = bro_config_source_current (self->config);
  g_autoptr (GPtrArray) entries = bro_config_drives (config);
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_dto_free);
  g_autoptr (GPtrArray) placeholders = g_ptr_array_new_with_free_func ((GDestroyNotify) drive_free);
  for (guint i = 0; i < self->drives->len; i++)
    {
      Drive *d = self->drives->pdata[i];
      if (d->connected || config_id (self, entries, d))
        g_ptr_array_add (list, d->shown ? bro_drive_dto_copy (d->shown) : dto_of (self, config, entries, d));
    }
  /* The configured entries without a connected drive: an id from the stored drive with that entry (the smallest), else
   * from the entry's match. */
  for (guint e = 0; e < entries->len; e++)
    {
      BroDriveEntry *entry = entries->pdata[e];
      gboolean have = FALSE;
      for (guint i = 0; i < list->len && !have; i++)
        have = g_strcmp0 (((BroDriveDto *) list->pdata[i])->config_id, entry->id) == 0;
      if (have)
        continue;
      const char *id = NULL;
      GHashTableIter it;
      gpointer value;
      g_hash_table_iter_init (&it, self->stored);
      while (g_hash_table_iter_next (&it, NULL, &value))
        {
          BroDriveRecord *r = value;
          if (g_strcmp0 (r->config_id, entry->id) == 0 && (!id || strcmp (r->id, id) < 0))
            id = r->id;
        }
      g_autofree char *computed = id ? NULL : bro_drive_join_drive_id (entry->match->drive_name, entry->match->device_path);
      Drive *d = drive_new (id ? id : computed, entry->id);
      g_ptr_array_add (placeholders, d);
      g_ptr_array_add (list, dto_of (self, config, entries, d));
    }
  return list;
}

void
bro_drive_service_rescan (BroDriveService *self, BroDriveDone done, gpointer data, GDestroyNotify destroy)
{
  scan (self, TRUE, done, data, destroy);
}

typedef struct {
  BroDriveService *self; /* a reference */
  Waiter *waiter;
  GPtrArray *devices; /* char * */
  gboolean eject;     /* else close trays */
  gboolean ignore_failures;
} Command;

static void
command_free (Command *c)
{
  g_object_unref (c->self);
  waiter_free (c->waiter);
  g_ptr_array_unref (c->devices);
  g_free (c);
}

static gboolean
command_work (gpointer data, BroBroError **error)
{
  Command *c = data;
  for (guint i = 0; i < c->devices->len; i++)
    {
      const char *device = c->devices->pdata[i];
      BroBroError *failure = NULL;
      gboolean ok = c->eject ? bro_drive_control_eject (c->self->control, device, &failure)
                             : bro_drive_control_close_tray (c->self->control, device, &failure);
      if (ok)
        continue;
      if (c->ignore_failures)
        {
          g_clear_pointer (&failure, bro_bro_error_free); /* the check after the scan says which trays stayed open */
          continue;
        }
      *error = failure;
      return FALSE;
    }
  return TRUE;
}

static void
command_done (gpointer data, const BroBroError *error)
{
  Command *c = data;
  if (!error)
    scan_in (c->self, c->eject ? SCAN_AFTER_EJECT_S : SCAN_AFTER_TRAY_S);
  if (c->waiter->done)
    c->waiter->done (error, c->waiter->data);
}

typedef struct {
  Waiter *waiter;
  BroBroError *error;
} Failed;

static void
failed_free (Failed *f)
{
  waiter_free (f->waiter);
  bro_bro_error_free (f->error);
  g_free (f);
}

static void
failed_now (gpointer data)
{
  Failed *f = data;
  if (f->waiter->done)
    f->waiter->done (f->error, f->waiter->data);
}

static void
ask_tray (BroDriveService *self, const char *id, gint64 at_ms)
{
  for (guint i = 0; i < self->tray_asked->len; i++)
    if (strcmp (((TrayAsk *) self->tray_asked->pdata[i])->drive, id) == 0)
      {
        g_ptr_array_remove_index (self->tray_asked, i);
        break;
      }
  TrayAsk *t = g_new0 (TrayAsk, 1);
  t->drive = g_strdup (id);
  t->at_ms = at_ms;
  g_ptr_array_add (self->tray_asked, t);
}

/* One drive's command; drive.notFound (posted) when it isn't connected. */
static void
drive_command (BroDriveService *self, const char *drive_id, gboolean eject, BroDriveDone done, gpointer data, GDestroyNotify destroy)
{
  Waiter *w = g_new0 (Waiter, 1);
  w->done = done;
  w->data = data;
  w->destroy = destroy;
  Drive *d = find_drive (self, drive_id);
  if (!d || !d->connected)
    {
      Failed *f = g_new0 (Failed, 1);
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "driveId", bro_json_value_new_string (drive_id));
      f->waiter = w;
      f->error = bro_bro_error_new (bro_message_code_wire (BRO_MSG_DRIVE_NOT_FOUND), params, NULL);
      bro_engine_executor_post (self->executor, failed_now, f, (GDestroyNotify) failed_free);
      return;
    }
  if (!eject)
    ask_tray (self, d->id, bro_engine_clock_now (self->clock).unix_milliseconds);
  Command *c = g_new0 (Command, 1);
  c->self = g_object_ref (self);
  c->waiter = w;
  c->devices = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (c->devices, g_strdup (device_of (d)));
  c->eject = eject;
  bro_engine_executor_worker (self->executor, command_work, command_done, c, (GDestroyNotify) command_free);
}

void
bro_drive_service_eject (BroDriveService *self, const char *drive_id, BroDriveDone done, gpointer data, GDestroyNotify destroy)
{
  drive_command (self, drive_id, TRUE, done, data, destroy);
}

void
bro_drive_service_close_tray (BroDriveService *self, const char *drive_id, BroDriveDone done, gpointer data, GDestroyNotify destroy)
{
  drive_command (self, drive_id, FALSE, done, data, destroy);
}

void
bro_drive_service_close_all (BroDriveService *self, BroDriveDone done, gpointer data, GDestroyNotify destroy)
{
  Command *c = g_new0 (Command, 1);
  gint64 now = bro_engine_clock_now (self->clock).unix_milliseconds;
  c->self = g_object_ref (self);
  c->waiter = g_new0 (Waiter, 1);
  c->waiter->done = done;
  c->waiter->data = data;
  c->waiter->destroy = destroy;
  c->devices = g_ptr_array_new_with_free_func (g_free);
  c->ignore_failures = TRUE;
  for (guint i = 0; i < self->drives->len; i++)
    {
      Drive *d = self->drives->pdata[i];
      if (d->connected && (d->state == BRO_DRIVE_STATUS_EMPTY || d->state == BRO_DRIVE_STATUS_TRAY_OPEN) && device_of (d)[0])
        {
          ask_tray (self, d->id, now);
          g_ptr_array_add (c->devices, g_strdup (device_of (d)));
        }
    }
  bro_engine_executor_worker (self->executor, command_work, command_done, c, (GDestroyNotify) command_free);
}

/* ---- DriveLookup ---- */

static BroDriveLocation *
find_vfunc (BroDriveLookup *lookup, const char *drive_id)
{
  BroDriveService *self = BRO_DRIVE_SERVICE (lookup);
  BroDriveLocation *l;
  g_mutex_lock (&self->lock);
  l = bro_drive_location_copy (g_hash_table_lookup (self->snapshot, drive_id));
  g_mutex_unlock (&self->lock);
  return l;
}

static void
drive_lookup_iface_init (BroDriveLookupInterface *iface)
{
  iface->find = find_vfunc;
}
