/* bro-library-service.c */
#include "bro-library-service.h"
#include "bro-config.h"
#include "bro-conflict-namer.h"
#include "bro-library-marker.h"
#include "bro-message-code.h"
#include "bro-sha256-hasher.h"
#include "bro-sha256-sums.h"

#include <string.h>

#define FILE_MODE 0644
#define STAGING ".bromelia"
#define QUARANTINE_FOLDER "_Quarantine"
#define CHECK_EVERY_SECONDS 30
#define SUMS "SHA256SUMS"

static const BroMessageCode reason_codes[] = { BRO_MSG_LIBRARY_OFFLINE, BRO_MSG_LIBRARY_NOT_ATTACHED, BRO_MSG_LIBRARY_MARKER_MISMATCH };

/* A library of the configuration, with what its last check found. Shared with the checks out for it. */
typedef struct {
  char *id;
  char *name;
  char *path;
  gboolean known;
  gboolean online;
  BroBroMessage *reason;
  gboolean has_free;
  gint64 free;
  gboolean has_seen;
  BroInstant seen;
  gboolean in_flight;
} Entry;

/* What a marker check found. */
typedef struct {
  BroBroMessage *reason; /* NULL: online */
  gboolean has_free;
  gint64 free;
  gboolean has_seen;
  BroInstant seen;
} Found;

typedef struct {
  BroLibraryDone done;
  gpointer data;
  GDestroyNotify destroy;
} Waiter;

/* Checks applied together, in configuration order, when all have come back or at the next tick. */
typedef struct {
  GPtrArray *pending;   /* Entry * (references) */
  GHashTable *results;  /* Entry * → Found * */
  GPtrArray *waiters;   /* Waiter * */
} Round;

/* The snapshot the steps read: one per library. */
typedef struct {
  char *name;
  char *path;
  BroBroMessage *reason; /* NULL: online */
} Snap;

struct _BroLibraryService {
  GObject parent_instance;
  BroEngineExecutor *executor;
  BroEventBus *bus;
  BroProblemService *problems;
  BroScheduler *scheduler;
  BroConfigSource *config;
  BroEngineClock *clock;
  BroFileSystem *fs;
  BroLibraryRepository *rows;
  BroUnitRepository *units;
  BroCrashPointFunc crash;
  gpointer crash_data;
  GPtrArray *entries; /* Entry * (references), in configuration order */
  Round *round;       /* a reference, or NULL */
  BroEngineTimer *timer;
  BroSubscription *subscription;
  GMutex lock;
  GHashTable *snapshot;   /* id → Snap * */
  GHashTable *committing; /* unit id → itself */
};

static void library_port_iface_init (BroLibraryPortInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroLibraryService, bro_library_service, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_LIBRARY_PORT, library_port_iface_init))

/* ---- small values ---- */

static void
entry_clear (gpointer data)
{
  Entry *e = data;
  g_free (e->id);
  g_free (e->name);
  g_free (e->path);
  bro_bro_message_free (e->reason);
}

static Entry *
entry_new (const char *id, const char *name, const char *path)
{
  Entry *e = g_rc_box_new0 (Entry);
  e->id = g_strdup (id);
  e->name = g_strdup (name);
  e->path = g_strdup (path);
  return e;
}

static Entry *
entry_ref (Entry *e)
{
  return g_rc_box_acquire (e);
}

static void
entry_unref (gpointer e)
{
  g_rc_box_release_full (e, entry_clear);
}

static gpointer
entry_copy (gconstpointer e, gpointer data)
{
  return g_rc_box_acquire ((gpointer) e);
}

static void
found_free (gpointer data)
{
  Found *f = data;
  if (!f)
    return;
  bro_bro_message_free (f->reason);
  g_free (f);
}

static void
waiter_free (gpointer data)
{
  Waiter *w = data;
  if (w->destroy)
    w->destroy (w->data);
  g_free (w);
}

static void
round_clear (gpointer data)
{
  Round *r = data;
  g_ptr_array_unref (r->pending);
  g_hash_table_unref (r->results);
  g_ptr_array_unref (r->waiters);
}

static Round *
round_new (void)
{
  Round *r = g_rc_box_new0 (Round);
  r->pending = g_ptr_array_new_with_free_func (entry_unref);
  r->results = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, found_free);
  r->waiters = g_ptr_array_new_with_free_func (waiter_free);
  return r;
}

static Round *
round_ref (Round *r)
{
  return g_rc_box_acquire (r);
}

static void
round_unref (gpointer r)
{
  g_rc_box_release_full (r, round_clear);
}

static void
snap_free (gpointer data)
{
  Snap *s = data;
  g_free (s->name);
  g_free (s->path);
  bro_bro_message_free (s->reason);
  g_free (s);
}

static BroBroMessage *
message (BroMessageCode code, BroSeverity severity, const char *k1, const char *v1, const char *k2, const char *v2, const char *k3, const char *v3)
{
  BroJsonValue *params = bro_json_value_new_object ();
  if (k1)
    bro_json_value_set (params, k1, bro_json_value_new_string (v1));
  if (k2)
    bro_json_value_set (params, k2, bro_json_value_new_string (v2));
  if (k3)
    bro_json_value_set (params, k3, bro_json_value_new_string (v3));
  return bro_bro_message_new (code, params, severity);
}

static BroBroMessage *
offline (const char *name, const char *path)
{
  return message (BRO_MSG_LIBRARY_OFFLINE, BRO_SEVERITY_WARNING, "library", name, "path", path, NULL, NULL);
}

/* Sets @error to @m as an error (takes @m); FALSE, for returning. */
static gboolean
fail (BroBroError **error, BroBroMessage *m)
{
  if (error)
    *error = bro_bro_message_to_error (m, NULL);
  bro_bro_message_free (m);
  return FALSE;
}

static gboolean
fail_path (BroBroError **error, BroMessageCode code, const char *path)
{
  return fail (error, message (code, BRO_SEVERITY_ERROR, "path", path, NULL, NULL, NULL, NULL));
}

static gboolean
messages_equal (const BroBroMessage *a, const BroBroMessage *b)
{
  if (!a || !b)
    return a == b;
  return a->code == b->code && a->severity == b->severity && bro_json_value_equal (a->params, b->params);
}

static BroProblemScope *
scope_of (const char *id)
{
  return bro_problem_scope_new (BRO_PROBLEM_SCOPE_KIND_LIBRARY, id);
}

static char *
parent_of (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash == path ? g_strdup ("/") : g_strndup (path, (gsize) (slash - path));
}

static char *
last_part (const char *path)
{
  g_autofree char *trimmed = g_strdup (path);
  gsize n = strlen (trimmed);
  const char *slash;
  while (n > 1 && trimmed[n - 1] == '/')
    trimmed[--n] = '\0';
  slash = strrchr (trimmed, '/');
  return g_strdup (slash ? slash + 1 : trimmed);
}

/* Strictly below the root, and not in its .bromelia folder. */
static gboolean
inside (const char *root, const char *path)
{
  g_autofree char *prefix = g_strconcat (root, "/", NULL);
  g_autofree char *staging = g_strconcat (root, "/" STAGING, NULL);
  g_autofree char *staging_prefix = g_strconcat (staging, "/", NULL);
  return g_str_has_prefix (path, prefix) && strcmp (path, staging) != 0 && !g_str_has_prefix (path, staging_prefix);
}

/* The bytes as text (g_free it); "" for none. */
static char *
text_of (GBytes *bytes)
{
  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);
  return size ? g_strndup (data, size) : g_strdup ("");
}

static gint
ordinal (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static gint
by_name (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroDirectoryEntry *const *) a)->name, (*(BroDirectoryEntry *const *) b)->name);
}

/* ---- the object ---- */

static void
bro_library_service_finalize (GObject *object)
{
  BroLibraryService *self = BRO_LIBRARY_SERVICE (object);
  bro_library_service_stop (self);
  g_clear_pointer (&self->round, round_unref);
  g_ptr_array_unref (self->entries);
  g_hash_table_unref (self->snapshot);
  g_hash_table_unref (self->committing);
  g_mutex_clear (&self->lock);
  g_object_unref (self->executor);
  g_object_unref (self->bus);
  g_object_unref (self->problems);
  g_object_unref (self->scheduler);
  g_object_unref (self->config);
  g_object_unref (self->clock);
  g_object_unref (self->fs);
  g_object_unref (self->rows);
  g_object_unref (self->units);
  G_OBJECT_CLASS (bro_library_service_parent_class)->finalize (object);
}

static void
bro_library_service_class_init (BroLibraryServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_library_service_finalize;
}

static void
bro_library_service_init (BroLibraryService *self)
{
  self->entries = g_ptr_array_new_with_free_func (entry_unref);
  self->snapshot = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, snap_free);
  self->committing = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_mutex_init (&self->lock);
}

BroLibraryService *
bro_library_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroScheduler *scheduler, BroConfigSource *config,
                         BroEngineClock *clock, BroFileSystem *fs, BroLibraryRepository *libraries, BroUnitRepository *units, BroCrashPointFunc crash,
                         gpointer crash_data)
{
  BroLibraryService *self = g_object_new (BRO_TYPE_LIBRARY_SERVICE, NULL);
  self->executor = g_object_ref (executor);
  self->bus = g_object_ref (bus);
  self->problems = g_object_ref (problems);
  self->scheduler = g_object_ref (scheduler);
  self->config = g_object_ref (config);
  self->clock = g_object_ref (clock);
  self->fs = g_object_ref (fs);
  self->rows = g_object_ref (libraries);
  self->units = g_object_ref (units);
  self->crash = crash;
  self->crash_data = crash_data;
  return self;
}

/* ---- the snapshot ---- */

static void
update_snapshot (BroLibraryService *self)
{
  GHashTable *snapshot = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, snap_free);
  for (guint i = 0; i < self->entries->len; i++)
    {
      Entry *e = self->entries->pdata[i];
      Snap *s = g_new0 (Snap, 1);
      s->name = g_strdup (e->name);
      s->path = g_strdup (e->path);
      s->reason = e->known ? (e->reason ? bro_bro_message_copy (e->reason) : NULL) : offline (e->name, e->path);
      g_hash_table_replace (snapshot, g_strdup (e->id), s);
    }
  g_mutex_lock (&self->lock);
  g_hash_table_unref (self->snapshot);
  self->snapshot = snapshot;
  g_mutex_unlock (&self->lock);
}

/* The library's name and root, from the snapshot: library.notFound, or why it is offline. */
static gboolean
online (BroLibraryService *self, const char *library_id, char **name, char **path, BroBroError **error)
{
  Snap *s;
  BroBroMessage *reason = NULL;
  gboolean found;
  g_mutex_lock (&self->lock);
  s = g_hash_table_lookup (self->snapshot, library_id);
  found = s != NULL;
  if (s && s->reason)
    reason = bro_bro_message_copy (s->reason);
  else if (s)
    {
      *name = g_strdup (s->name);
      *path = g_strdup (s->path);
    }
  g_mutex_unlock (&self->lock);
  if (!found)
    return fail (error, bro_bro_message_new (BRO_MSG_LIBRARY_NOT_FOUND, NULL, BRO_SEVERITY_ERROR));
  if (reason)
    return fail (error, reason);
  return TRUE;
}

/* ---- marker checks (workers) ---- */

static gboolean
is_folder (BroLibraryService *self, const char *path)
{
  g_autoptr (BroFileInfo) info = NULL;
  if (!bro_file_system_exists (self->fs, path))
    return FALSE;
  info = bro_file_system_stat (self->fs, path, NULL);
  return info && info->is_directory;
}

static BroLibraryMarker *
decode_marker (BroLibraryService *self, const char *marker_path)
{
  g_autoptr (GBytes) bytes = bro_file_system_read (self->fs, marker_path, NULL);
  return bytes ? bro_library_marker_decode (bytes) : NULL;
}

static gboolean
free_bytes (BroLibraryService *self, const char *path, gint64 *out)
{
  g_autoptr (BroBroError) e = NULL;
  g_autoptr (BroVolumeInfo) v = bro_file_system_volume (self->fs, path, &e);
  if (!v)
    return FALSE;
  *out = v->free_bytes;
  return TRUE;
}

/* NULL in *reason when the folder holds the stored marker (adopting a marker that isn't stored yet); otherwise why the
 * library is offline. FALSE (and @error) when storing an adopted marker failed. */
static gboolean
marker_reason (BroLibraryService *self, const char *id, const char *name, const char *path, const BroLibraryRecord *row, BroBroMessage **reason,
               BroBroError **error)
{
  g_autofree char *marker_path = g_strconcat (path, "/" BRO_LIBRARY_MARKER_FILE_NAME, NULL);
  g_autoptr (BroLibraryMarker) marker = NULL;
  *reason = NULL;
  if (!bro_file_system_exists (self->fs, path))
    {
      *reason = offline (name, path);
      return TRUE;
    }
  if (!bro_file_system_exists (self->fs, marker_path))
    {
      *reason = row ? offline (name, path) : message (BRO_MSG_LIBRARY_NOT_ATTACHED, BRO_SEVERITY_WARNING, "path", path, NULL, NULL, NULL, NULL);
      return TRUE;
    }
  marker = decode_marker (self, marker_path);
  if (!marker)
    {
      *reason = offline (name, path);
      return TRUE;
    }
  if (!row || strcmp (row->path, path) != 0)
    {
      BroInstant now = bro_engine_clock_now (self->clock);
      g_autoptr (BroLibraryRecord) adopted = bro_library_record_new ();
      adopted->id = g_strdup (id);
      adopted->name = g_strdup (name);
      adopted->path = g_strdup (path);
      adopted->marker_id = g_strdup (marker->id);
      adopted->attached_at = marker->has_created ? marker->created : now;
      adopted->online = TRUE;
      adopted->has_last_seen_at = TRUE;
      adopted->last_seen_at = now;
      return bro_library_repository_upsert (self->rows, adopted, error);
    }
  if (strcmp (row->marker_id, marker->id) != 0)
    *reason = message (BRO_MSG_LIBRARY_MARKER_MISMATCH, BRO_SEVERITY_ERROR, "path", path, "found", marker->id, "expected", row->marker_id);
  return TRUE;
}

/* The marker check of one library; stores what it found. */
static Found *
inspect (BroLibraryService *self, const char *id, const char *name, const char *path)
{
  g_autoptr (BroBroError) e = NULL;
  g_autoptr (BroLibraryRecord) row = bro_library_repository_get (self->rows, id, &e);
  BroBroMessage *reason = NULL;
  Found *found = g_new0 (Found, 1);
  BroInstant now;
  if (e || !marker_reason (self, id, name, path, row, &reason, &e))
    {
      bro_bro_message_free (reason);
      reason = offline (name, path);
    }
  now = bro_engine_clock_now (self->clock);
  bro_library_repository_set_seen (self->rows, id, reason == NULL, now, NULL);
  found->reason = reason;
  if (!reason)
    {
      found->has_free = free_bytes (self, path, &found->free);
      found->has_seen = TRUE;
      found->seen = now;
    }
  else if (row && row->has_last_seen_at)
    {
      found->has_seen = TRUE;
      found->seen = row->last_seen_at;
    }
  return found;
}

static gboolean
check_marker (BroLibraryService *self, const char *id, const char *name, const char *path, BroBroError **error)
{
  g_autoptr (BroBroError) e = NULL;
  g_autoptr (BroLibraryRecord) row = bro_library_repository_get (self->rows, id, &e);
  BroBroMessage *reason = NULL;
  if (e || !marker_reason (self, id, name, path, row, &reason, &e))
    {
      bro_bro_message_free (reason);
      reason = offline (name, path);
    }
  return reason ? fail (error, reason) : TRUE;
}

/* ---- marker checks (executor) ---- */

static BroLibraryDto *
dto_of (const Entry *e)
{
  BroLibraryDto *d = bro_library_dto_new ();
  d->id = g_strdup (e->id);
  d->name = g_strdup (e->name);
  d->path = g_strdup (e->path);
  d->online = e->known && e->online;
  d->reason = e->known && e->reason ? bro_bro_message_copy (e->reason) : NULL;
  d->has_free_bytes = e->online && e->has_free;
  d->free_bytes = e->free;
  d->has_last_seen_at = e->has_seen;
  d->last_seen_at = e->seen;
  return d;
}

static void
apply (BroLibraryService *self, Entry *e, const Found *found)
{
  gboolean changed = !e->known || e->online != (found->reason == NULL) || !messages_equal (e->reason, found->reason);
  g_autoptr (BroProblemScope) scope = scope_of (e->id);
  e->known = TRUE;
  e->online = found->reason == NULL;
  bro_bro_message_free (e->reason);
  e->reason = found->reason ? bro_bro_message_copy (found->reason) : NULL;
  e->has_free = found->has_free;
  e->free = found->free;
  if (found->has_seen)
    {
      e->has_seen = TRUE;
      e->seen = found->seen;
    }
  update_snapshot (self);
  if (!changed)
    return;
  {
    g_autoptr (BroBroError) reason = found->reason ? bro_bro_message_to_error (found->reason, NULL) : NULL;
    g_autoptr (BroLibraryDto) dto = dto_of (e);
    g_autoptr (BroJsonValue) data = bro_json_value_new_object ();
    bro_scheduler_set_library_offline (self->scheduler, e->id, reason);
    bro_json_value_set (data, "library", bro_library_dto_json (dto));
    bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_LIBRARY_CHANGED, data));
  }
  for (guint i = 0; i < G_N_ELEMENTS (reason_codes); i++)
    if (!found->reason || found->reason->code != reason_codes[i])
      bro_problem_service_clear (self->problems, reason_codes[i], scope);
  if (found->reason)
    {
      BroProblemAction action = found->reason->code == BRO_MSG_LIBRARY_NOT_ATTACHED ? BRO_PROBLEM_ACTION_ATTACH_LIBRARY : BRO_PROBLEM_ACTION_OPEN_SETTINGS;
      bro_problem_dto_free (bro_problem_service_raise (self->problems, found->reason, scope, &action, 1));
    }
}

static gboolean
contains (GPtrArray *entries, Entry *e)
{
  return g_ptr_array_find (entries, e, NULL);
}

static void
set_seen_offline_free (gpointer data)
{
  g_free (data);
}

typedef struct {
  BroLibraryService *self;
  char *id;
  BroInstant at;
} SeenOffline;

static gboolean
seen_offline_work (gpointer data, BroBroError **error)
{
  SeenOffline *s = data;
  bro_library_repository_set_seen (s->self->rows, s->id, FALSE, s->at, NULL);
  return TRUE;
}

static void
seen_offline_done (gpointer data, const BroBroError *error)
{
}

static void
seen_offline_destroy (gpointer data)
{
  SeenOffline *s = data;
  g_object_unref (s->self);
  g_free (s->id);
  set_seen_offline_free (s);
}

/* Applies what came back in configuration order; libraries still out are offline. */
static void
close_round (BroLibraryService *self, Round *round)
{
  g_autoptr (GPtrArray) waiters = NULL;
  g_autoptr (GPtrArray) entries = g_ptr_array_copy (self->entries, entry_copy, NULL);
  round_ref (round);
  if (self->round == round)
    g_clear_pointer (&self->round, round_unref);
  g_ptr_array_set_free_func (entries, entry_unref);
  for (guint i = 0; i < entries->len; i++)
    {
      Entry *e = entries->pdata[i];
      Found *found = g_hash_table_lookup (round->results, e);
      if (found)
        apply (self, e, found);
      else if (g_ptr_array_find (round->pending, e, NULL))
        {
          Found stuck = { offline (e->name, e->path), FALSE, 0, e->has_seen, e->seen };
          SeenOffline *s = g_new0 (SeenOffline, 1);
          s->self = g_object_ref (self);
          s->id = g_strdup (e->id);
          s->at = bro_engine_clock_now (self->clock);
          bro_engine_executor_worker (self->executor, seen_offline_work, seen_offline_done, s, seen_offline_destroy);
          apply (self, e, &stuck);
          bro_bro_message_free (stuck.reason);
        }
    }
  waiters = round->waiters;
  round->waiters = g_ptr_array_new_with_free_func (waiter_free);
  for (guint i = 0; i < waiters->len; i++)
    {
      Waiter *w = waiters->pdata[i];
      if (w->done)
        w->done (NULL, w->data);
    }
  round_unref (round);
}

typedef struct {
  BroLibraryService *self;
  Entry *entry;
  Round *round;
  char *id;
  char *name;
  char *path;
  Found *found;
} Check;

static void
check_free (gpointer data)
{
  Check *c = data;
  g_object_unref (c->self);
  entry_unref (c->entry);
  round_unref (c->round);
  g_free (c->id);
  g_free (c->name);
  g_free (c->path);
  found_free (c->found);
  g_free (c);
}

static gboolean
check_work (gpointer data, BroBroError **error)
{
  Check *c = data;
  c->found = inspect (c->self, c->id, c->name, c->path);
  return TRUE;
}

static void
check_done (gpointer data, const BroBroError *error)
{
  Check *c = data;
  BroLibraryService *self = c->self;
  gboolean current = contains (self->entries, c->entry);
  guint index;
  c->entry->in_flight = FALSE;
  if (self->round == c->round && g_ptr_array_find (c->round->pending, c->entry, &index))
    {
      g_ptr_array_remove_index (c->round->pending, index);
      if (current)
        g_hash_table_replace (c->round->results, c->entry, g_steal_pointer (&c->found));
      if (c->round->pending->len == 0)
        close_round (self, c->round);
    }
  else if (current)
    apply (self, c->entry, c->found);
}

/* Checks every library that has no check out, in the open round (a new one when there is none); @waiter (may be
 * NULL; taken) runs when that round is applied. */
static void
start_round (BroLibraryService *self, Waiter *waiter)
{
  Round *round;
  if (!self->round)
    self->round = round_new ();
  round = round_ref (self->round);
  if (waiter)
    g_ptr_array_add (round->waiters, waiter);
  for (guint i = 0; i < self->entries->len; i++)
    {
      Entry *e = self->entries->pdata[i];
      Check *c;
      if (e->in_flight)
        continue;
      e->in_flight = TRUE;
      g_ptr_array_add (round->pending, entry_ref (e));
      c = g_new0 (Check, 1);
      c->self = g_object_ref (self);
      c->entry = entry_ref (e);
      c->round = round_ref (round);
      c->id = g_strdup (e->id);
      c->name = g_strdup (e->name);
      c->path = g_strdup (e->path);
      bro_engine_executor_worker (self->executor, check_work, check_done, c, check_free);
    }
  if (round->pending->len == 0)
    close_round (self, round);
  round_unref (round);
}

static Entry *
find (BroLibraryService *self, const char *id)
{
  for (guint i = 0; i < self->entries->len; i++)
    if (strcmp (((Entry *) self->entries->pdata[i])->id, id) == 0)
      return self->entries->pdata[i];
  return NULL;
}

static void
sync_entries (BroLibraryService *self)
{
  g_autoptr (BroConfig) config = bro_config_source_current (self->config);
  g_autoptr (GPtrArray) wanted = bro_config_libraries (config);
  GPtrArray *ordered = g_ptr_array_new_with_free_func (entry_unref);
  for (guint i = 0; i < self->entries->len; i++)
    {
      Entry *e = self->entries->pdata[i];
      gboolean kept = FALSE;
      for (guint j = 0; j < wanted->len && !kept; j++)
        kept = strcmp (((BroLibraryEntry *) wanted->pdata[j])->id, e->id) == 0;
      if (!kept)
        {
          g_autoptr (BroProblemScope) scope = scope_of (e->id);
          bro_scheduler_set_library_offline (self->scheduler, e->id, NULL);
          for (guint k = 0; k < G_N_ELEMENTS (reason_codes); k++)
            bro_problem_service_clear (self->problems, reason_codes[k], scope);
        }
    }
  for (guint j = 0; j < wanted->len; j++)
    {
      BroLibraryEntry *w = wanted->pdata[j];
      const char *name = *w->name ? w->name : w->id;
      Entry *e = find (self, w->id);
      if (e && strcmp (e->path, w->path) == 0)
        {
          g_free (e->name);
          e->name = g_strdup (name);
          g_ptr_array_add (ordered, entry_ref (e));
        }
      else
        g_ptr_array_add (ordered, entry_new (w->id, name, w->path));
    }
  if (self->round)
    for (guint i = self->round->pending->len; i > 0; i--)
      if (!contains (ordered, self->round->pending->pdata[i - 1]))
        g_ptr_array_remove_index (self->round->pending, i - 1);
  g_ptr_array_unref (self->entries);
  self->entries = ordered;
  update_snapshot (self);
  if (self->round && self->round->pending->len == 0)
    close_round (self, self->round);
}

static void
on_config_changed (BroEngineEvent *event, gpointer data)
{
  BroLibraryService *self = data;
  sync_entries (self);
  start_round (self, NULL);
}

static void
tick (gpointer data)
{
  BroLibraryService *self = data;
  if (self->round)
    close_round (self, self->round);
  start_round (self, NULL);
}

static Waiter *
waiter_new (BroLibraryDone done, gpointer data, GDestroyNotify destroy)
{
  Waiter *w = g_new0 (Waiter, 1);
  w->done = done;
  w->data = data;
  w->destroy = destroy;
  return w;
}

/* ---- start ---- */

typedef struct {
  BroLibraryService *self;
  GPtrArray *rows;
  Waiter *waiter;
} Start;

static void
start_free (gpointer data)
{
  Start *s = data;
  g_object_unref (s->self);
  g_clear_pointer (&s->rows, g_ptr_array_unref);
  if (s->waiter)
    waiter_free (s->waiter);
  g_free (s);
}

static gboolean
start_work (gpointer data, BroBroError **error)
{
  Start *s = data;
  s->rows = bro_library_repository_all (s->self->rows, error);
  return s->rows != NULL;
}

static void
start_done (gpointer data, const BroBroError *error)
{
  Start *s = data;
  BroLibraryService *self = s->self;
  static const BroEngineEventType types[] = { BRO_ENGINE_EVENT_TYPE_CONFIG_CHANGED };
  BroTimerSchedule every = { BRO_TIMER_SCHEDULE_EVERY, { 0 }, { CHECK_EVERY_SECONDS } };
  if (error)
    {
      if (s->waiter->done)
        s->waiter->done (error, s->waiter->data);
      return;
    }
  sync_entries (self);
  for (guint i = 0; i < self->entries->len; i++)
    {
      Entry *e = self->entries->pdata[i];
      for (guint j = 0; j < s->rows->len; j++)
        {
          BroLibraryRecord *row = s->rows->pdata[j];
          if (strcmp (row->id, e->id) == 0 && strcmp (row->path, e->path) == 0 && row->has_last_seen_at)
            {
              e->has_seen = TRUE;
              e->seen = row->last_seen_at;
            }
        }
    }
  self->subscription = bro_event_bus_subscribe (self->bus, types, G_N_ELEMENTS (types), on_config_changed, self, NULL);
  self->timer = bro_engine_clock_timer (self->clock, &every, tick, self, NULL);
  start_round (self, g_steal_pointer (&s->waiter));
}

void
bro_library_service_start (BroLibraryService *self, BroLibraryDone done, gpointer data, GDestroyNotify destroy)
{
  Start *s = g_new0 (Start, 1);
  s->self = g_object_ref (self);
  s->waiter = waiter_new (done, data, destroy);
  bro_engine_executor_worker (self->executor, start_work, start_done, s, start_free);
}

void
bro_library_service_stop (BroLibraryService *self)
{
  if (self->timer)
    {
      bro_engine_timer_cancel (self->timer);
      g_clear_object (&self->timer);
    }
  if (self->subscription)
    {
      bro_subscription_cancel (self->subscription);
      g_clear_pointer (&self->subscription, g_object_unref);
    }
}

GPtrArray *
bro_library_service_libraries (BroLibraryService *self)
{
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_library_dto_free);
  for (guint i = 0; i < self->entries->len; i++)
    g_ptr_array_add (list, dto_of (self->entries->pdata[i]));
  return list;
}

typedef struct {
  BroLibraryService *self;
  Waiter *waiter;
} Later;

static void
check_action (gpointer data)
{
  Later *l = data;
  start_round (l->self, g_steal_pointer (&l->waiter));
}

static void
later_free (gpointer data)
{
  Later *l = data;
  g_object_unref (l->self);
  if (l->waiter)
    waiter_free (l->waiter);
  g_free (l);
}

void
bro_library_service_check (BroLibraryService *self, BroLibraryDone done, gpointer data, GDestroyNotify destroy)
{
  Later *l = g_new0 (Later, 1);
  l->self = g_object_ref (self);
  l->waiter = waiter_new (done, data, destroy);
  /* Never inside the call: the round may be applied at once. */
  bro_engine_executor_post (self->executor, check_action, l, later_free);
}

/* ---- attach ---- */

typedef struct {
  BroLibraryService *self;
  char *id;
  char *path;
  char *name;
  BroInstant now;
  gboolean has_free;
  gint64 free;
  BroLibraryDtoDone done;
  gpointer data;
  GDestroyNotify destroy;
} Attach;

static void
attach_free (gpointer data)
{
  Attach *a = data;
  if (a->destroy)
    a->destroy (a->data);
  g_object_unref (a->self);
  g_free (a->id);
  g_free (a->path);
  g_free (a->name);
  g_free (a);
}

static gboolean
attach_work (gpointer data, BroBroError **error)
{
  Attach *a = data;
  BroLibraryService *self = a->self;
  g_autofree char *marker_path = g_strconcat (a->path, "/" BRO_LIBRARY_MARKER_FILE_NAME, NULL);
  g_autoptr (BroLibraryMarker) marker = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (BroLibraryRecord) row = NULL;
  if (!is_folder (self, a->path))
    return fail_path (error, BRO_MSG_LIBRARY_PATH_MISSING, a->path);
  if (bro_file_system_exists (self->fs, marker_path))
    {
      g_autoptr (BroLibraryMarker) found = decode_marker (self, marker_path);
      return fail (error, message (BRO_MSG_LIBRARY_ALREADY_ATTACHED, BRO_SEVERITY_ERROR, "path", a->path, "library",
                                   found && *found->name ? found->name : a->id, NULL, NULL));
    }
  marker = bro_library_marker_make (a->id, a->path, a->name, a->now);
  bytes = bro_library_marker_encode (marker);
  if (!bro_file_system_write_atomically (self->fs, marker_path, bytes, FILE_MODE, error))
    return FALSE;
  row = bro_library_record_new ();
  row->id = g_strdup (a->id);
  row->name = g_strdup (a->name);
  row->path = g_strdup (a->path);
  row->marker_id = g_strdup (marker->id);
  row->attached_at = a->now;
  row->online = TRUE;
  row->has_last_seen_at = TRUE;
  row->last_seen_at = a->now;
  if (!bro_library_repository_upsert (self->rows, row, error))
    return FALSE;
  a->has_free = free_bytes (self, a->path, &a->free);
  return TRUE;
}

static void
attach_done (gpointer data, const BroBroError *error)
{
  Attach *a = data;
  BroLibraryService *self = a->self;
  Entry *e;
  Found found = { NULL, a->has_free, a->free, TRUE, a->now };
  if (error)
    {
      a->done (NULL, error, a->data);
      return;
    }
  e = find (self, a->id);
  if (!e || strcmp (e->path, a->path) != 0)
    {
      Entry *fresh = entry_new (a->id, e ? e->name : a->name, a->path);
      guint index;
      if (e && g_ptr_array_find (self->entries, e, &index))
        {
          g_ptr_array_insert (self->entries, (gint) index, fresh);
          g_ptr_array_remove_index (self->entries, index + 1);
        }
      else
        g_ptr_array_add (self->entries, fresh);
      e = fresh;
    }
  apply (self, e, &found);
  a->done (dto_of (e), NULL, a->data);
}

void
bro_library_service_attach (BroLibraryService *self, const char *library_id, const char *path, const char *name, BroLibraryDtoDone done,
                            gpointer data, GDestroyNotify destroy)
{
  Attach *a = g_new0 (Attach, 1);
  a->self = g_object_ref (self);
  a->id = g_strdup (library_id);
  a->path = g_strdup (path);
  a->name = *name ? g_strdup (name) : last_part (path);
  a->now = bro_engine_clock_now (self->clock);
  a->done = done;
  a->data = data;
  a->destroy = destroy;
  bro_engine_executor_worker (self->executor, attach_work, attach_done, a, attach_free);
}

/* ---- the port (any thread) ---- */

static BroLibraryHandle *
port_require (BroLibraryPort *port, const char *library_id, BroBroError **error)
{
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  if (!online (BRO_LIBRARY_SERVICE (port), library_id, &name, &path, error))
    return NULL;
  return bro_library_handle_new (library_id, path);
}

static char *
port_staging_directory (BroLibraryPort *port, const char *library_id, const BroId *job_id, BroBroError **error)
{
  BroLibraryService *self = BRO_LIBRARY_SERVICE (port);
  g_autoptr (BroLibraryHandle) handle = port_require (port, library_id, error);
  const char *parts[] = { STAGING, "staging", job_id->value };
  char *dir;
  if (!handle)
    return NULL;
  dir = g_strdup (handle->path);
  for (guint i = 0; i < G_N_ELEMENTS (parts); i++)
    {
      char *next = g_strconcat (dir, "/", parts[i], NULL);
      g_free (dir);
      dir = next;
      if (!bro_file_system_exists (self->fs, dir) && !bro_file_system_create_directory (self->fs, dir, TRUE, error))
        {
          g_free (dir);
          return NULL;
        }
    }
  return dir;
}

/* ---- the commit (workers) ---- */

static gboolean
crash_point (BroLibraryService *self, const char *point, BroBroError **error)
{
  return !self->crash || self->crash (point, self->crash_data, error);
}

static gboolean
claim (BroLibraryService *self, BroId unit_id, BroBroError **error)
{
  gboolean claimed;
  g_mutex_lock (&self->lock);
  claimed = !g_hash_table_contains (self->committing, unit_id.value);
  if (claimed)
    g_hash_table_add (self->committing, g_strdup (unit_id.value));
  g_mutex_unlock (&self->lock);
  return claimed ? TRUE : fail (error, message (BRO_MSG_COMMIT_IN_PROGRESS, BRO_SEVERITY_ERROR, "unit", unit_id.value, NULL, NULL, NULL, NULL));
}

static void
unclaim (BroLibraryService *self, BroId unit_id)
{
  g_mutex_lock (&self->lock);
  g_hash_table_remove (self->committing, unit_id.value);
  g_mutex_unlock (&self->lock);
}

/* Makes the folders from below the library's root down to @folder; never the root. */
static gboolean
ensure_folders (BroLibraryService *self, const char *root, const char *folder, BroBroError **error)
{
  g_auto (GStrv) parts = NULL;
  g_autofree char *current = g_strdup (root);
  if (strcmp (folder, root) == 0)
    return TRUE;
  if (!inside (root, folder))
    return fail (error, bro_bro_message_new (BRO_MSG_LIBRARY_NOT_FOUND, NULL, BRO_SEVERITY_ERROR));
  parts = g_strsplit (folder + strlen (root) + 1, "/", -1);
  for (guint i = 0; parts[i]; i++)
    {
      char *next = g_strconcat (current, "/", parts[i], NULL);
      g_free (current);
      current = next;
      if (!bro_file_system_exists (self->fs, current) && !bro_file_system_create_directory (self->fs, current, TRUE, error))
        return FALSE;
    }
  return TRUE;
}

/* The relative paths of every file below @folder, ordinal order. */
static gboolean
walk (BroLibraryService *self, const char *folder, const char *prefix, GPtrArray *out, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = bro_file_system_list (self->fs, folder, error);
  g_autoptr (GPtrArray) names = NULL;
  if (!entries)
    return FALSE;
  names = g_ptr_array_new ();
  for (guint i = 0; i < entries->len; i++)
    g_ptr_array_add (names, entries->pdata[i]);
  g_ptr_array_sort (names, by_name);
  for (guint i = 0; i < names->len; i++)
    {
      BroDirectoryEntry *e = names->pdata[i];
      g_autofree char *relative = *prefix ? g_strconcat (prefix, "/", e->name, NULL) : g_strdup (e->name);
      if (e->is_directory)
        {
          g_autofree char *sub = g_strconcat (folder, "/", e->name, NULL);
          if (!walk (self, sub, relative, out, error))
            return FALSE;
        }
      else
        g_ptr_array_add (out, g_steal_pointer (&relative));
    }
  return TRUE;
}

/* Nothing is written unless every item is in staging with SHA256SUMS, and nothing at the destination is in the way
 * (I5). */
static gboolean
precheck (BroLibraryService *self, const BroCommitIntent *intent, BroBroError **error)
{
  g_autofree char *sums = g_strconcat (intent->staging, "/" SUMS, NULL);
  if (!is_folder (self, intent->staging))
    return fail_path (error, BRO_MSG_FS_NOT_FOUND, intent->staging);
  if (!bro_file_system_exists (self->fs, sums))
    return fail_path (error, BRO_MSG_FS_NOT_FOUND, sums);
  for (guint i = 0; i < intent->items->len; i++)
    {
      BroCommitItem *item = intent->items->pdata[i];
      g_autofree char *from = g_strconcat (intent->staging, "/", item->from_path, NULL);
      if (!bro_file_system_exists (self->fs, from))
        return fail_path (error, BRO_MSG_FS_NOT_FOUND, from);
    }
  if (!intent->merge)
    return bro_file_system_exists (self->fs, intent->destination) ? fail_path (error, BRO_MSG_LIBRARY_FOLDER_EXISTS, intent->destination) : TRUE;
  for (guint i = 0; i < intent->items->len; i++)
    {
      BroCommitItem *item = intent->items->pdata[i];
      g_autoptr (GPtrArray) targets = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *to = g_strconcat (intent->destination, "/", item->to_path, NULL);
      if (item->is_dir)
        {
          g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
          g_autofree char *from = g_strconcat (intent->staging, "/", item->from_path, NULL);
          if (!walk (self, from, "", files, error))
            return FALSE;
          for (guint j = 0; j < files->len; j++)
            g_ptr_array_add (targets, g_strconcat (to, "/", (char *) files->pdata[j], NULL));
        }
      else
        g_ptr_array_add (targets, g_strdup (to));
      for (guint j = 0; j < targets->len; j++)
        if (bro_file_system_exists (self->fs, targets->pdata[j]) && !is_folder (self, targets->pdata[j]))
          return fail_path (error, BRO_MSG_FS_ALREADY_EXISTS, targets->pdata[j]);
    }
  return TRUE;
}

static char *
hash_of (BroLibraryService *self, const char *path, BroBroError **error)
{
  g_autoptr (BroByteStream) stream = bro_file_system_open_for_reading (self->fs, path, TRUE, error);
  g_autoptr (BroSha256Hasher) hasher = NULL;
  char *digest = NULL;
  if (!stream)
    return NULL;
  hasher = bro_sha256_hasher_new ();
  for (;;)
    {
      g_autoptr (GBytes) chunk = bro_byte_stream_read (stream, 1 << 20, error);
      gsize size;
      const guint8 *bytes;
      if (!chunk)
        break;
      bytes = g_bytes_get_data (chunk, &size);
      if (size == 0)
        {
          digest = bro_sha256_hasher_finish (hasher);
          break;
        }
      bro_sha256_hasher_update (hasher, bytes, size);
    }
  bro_byte_stream_close (stream);
  return digest;
}

/* One item: a file renamed, a folder merged; one already at its destination is recognised by its hash, never
 * replaced. */
static gboolean
move_item (BroLibraryService *self, const char *root, const BroCommitIntent *intent, const BroCommitItem *item, BroBroError **error)
{
  g_autofree char *from = g_strconcat (intent->staging, "/", item->from_path, NULL);
  g_autofree char *to = g_strconcat (intent->destination, "/", item->to_path, NULL);
  g_autofree char *to_parent = parent_of (to);
  gboolean there = bro_file_system_exists (self->fs, from);
  gboolean arrived = bro_file_system_exists (self->fs, to);
  if (item->is_dir)
    {
      if (there)
        {
          g_autoptr (GPtrArray) moved = NULL;
          if (!ensure_folders (self, root, to_parent, error))
            return FALSE;
          moved = bro_file_system_move_merging (self->fs, from, to, BRO_MOVE_POLICY_NEVER_REPLACE, NULL, NULL, error);
          return moved != NULL;
        }
      return arrived ? TRUE : fail_path (error, BRO_MSG_FS_NOT_FOUND, from);
    }
  if (there && !arrived)
    return ensure_folders (self, root, to_parent, error) && bro_file_system_rename (self->fs, from, to, error);
  if (!there && arrived)
    {
      g_autofree char *digest = NULL;
      if (!item->sha256)
        return TRUE;
      digest = hash_of (self, to, error);
      if (!digest)
        return FALSE;
      return strcmp (digest, item->sha256) == 0 ? TRUE : fail_path (error, BRO_MSG_FS_ALREADY_EXISTS, to);
    }
  if (there)
    return fail_path (error, BRO_MSG_FS_ALREADY_EXISTS, to);
  return fail_path (error, BRO_MSG_FS_NOT_FOUND, from);
}

/* The unit's lines of the destination's SHA256SUMS (a roll forward has no file list of its own). */
static GPtrArray *
from_sums (BroLibraryService *self, const BroCommitIntent *intent, BroBroError **error)
{
  g_autofree char *sums_path = g_strconcat (intent->destination, "/" SUMS, NULL);
  g_autoptr (GBytes) bytes = bro_file_system_read (self->fs, sums_path, error);
  g_autofree char *text = NULL;
  g_autoptr (GPtrArray) sums = NULL;
  GPtrArray *files;
  if (!bytes)
    return NULL;
  text = text_of (bytes);
  sums = bro_sha256_sums_parse (text);
  files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  for (guint i = 0; i < sums->len; i++)
    {
      BroSumEntry *s = sums->pdata[i];
      gboolean ours = FALSE;
      for (guint j = 0; j < intent->items->len && !ours; j++)
        {
          BroCommitItem *item = intent->items->pdata[j];
          g_autofree char *prefix = g_strconcat (item->to_path, "/", NULL);
          ours = item->is_dir ? g_str_has_prefix (s->path, prefix) : strcmp (s->path, item->to_path) == 0;
        }
      if (ours)
        {
          g_autofree char *full = g_strconcat (intent->destination, "/", s->path, NULL);
          g_autoptr (BroFileInfo) info = bro_file_system_stat (self->fs, full, error);
          BroUnitFile *f;
          if (!info)
            {
              g_ptr_array_unref (files);
              return NULL;
            }
          f = bro_unit_file_new ();
          f->path = g_strdup (s->path);
          f->size = info->size;
          f->sha256 = g_strdup (s->sha256);
          f->role = g_strdup ("title");
          g_ptr_array_add (files, f);
        }
    }
  return files;
}

/* Every folder from the library's root down to each committed file, and the staging folder's parent. */
static GPtrArray *
synced (const char *root, const BroCommitIntent *intent, GPtrArray *files)
{
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *folders = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (folders, parent_of (intent->staging));
  for (guint i = 0; i < files->len; i++)
    {
      BroUnitFile *f = files->pdata[i];
      g_autofree char *full = g_strconcat (intent->destination, "/", f->path, NULL);
      g_auto (GStrv) parts = g_strsplit (full + strlen (root) + 1, "/", -1);
      g_autofree char *folder = g_strdup (root);
      guint n = g_strv_length (parts);
      g_ptr_array_add (folders, g_strdup (folder));
      for (guint j = 0; j + 1 < n; j++)
        {
          char *next = g_strconcat (folder, "/", parts[j], NULL);
          g_free (folder);
          folder = next;
          g_ptr_array_add (folders, g_strdup (folder));
        }
    }
  g_ptr_array_sort (folders, ordinal);
  for (guint i = folders->len; i > 1; i--)
    if (strcmp (folders->pdata[i - 1], folders->pdata[i - 2]) == 0)
      g_ptr_array_remove_index (folders, i - 1);
  return folders;
}

/* Removes the staging folder's empty folders, and the folder itself once empty; anything else stays. */
static gboolean
remove_empty (BroLibraryService *self, const char *folder, BroBroError **error)
{
  g_autoptr (GPtrArray) entries = NULL;
  g_autoptr (GPtrArray) after = NULL;
  if (!is_folder (self, folder))
    return TRUE;
  entries = bro_file_system_list (self->fs, folder, error);
  if (!entries)
    return FALSE;
  for (guint i = 0; i < entries->len; i++)
    {
      BroDirectoryEntry *e = entries->pdata[i];
      g_autofree char *sub = g_strconcat (folder, "/", e->name, NULL);
      if (e->is_directory && !remove_empty (self, sub, error))
        return FALSE;
    }
  after = bro_file_system_list (self->fs, folder, error);
  if (!after)
    return FALSE;
  return after->len == 0 ? bro_file_system_remove (self->fs, folder, error) : TRUE;
}

static GPtrArray *
copy_files (GPtrArray *files)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  for (guint i = 0; i < files->len; i++)
    g_ptr_array_add (copy, bro_unit_file_copy (files->pdata[i]));
  return copy;
}

static int
by_seq (gconstpointer a, gconstpointer b)
{
  return (*(BroCommitItem *const *) a)->seq - (*(BroCommitItem *const *) b)->seq;
}

/* Steps 5 and 6 from wherever the intent is: the move, SHA256SUMS, the syncs, then the unit committed. @files: the
 * caller's (copied), or NULL for the destination's SHA256SUMS lines. */
static BroCommitResult *
complete (BroLibraryService *self, const char *root, const BroCommitIntent *intent, GPtrArray *files, BroBroError **error)
{
  g_autoptr (GPtrArray) pending = g_ptr_array_new ();
  g_autofree char *staging_sums = g_strconcat (intent->staging, "/" SUMS, NULL);
  g_autofree char *sums = g_strconcat (intent->destination, "/" SUMS, NULL);
  g_autoptr (GPtrArray) unit_files = NULL;
  g_autoptr (GPtrArray) folders = NULL;
  g_autoptr (BroUnitRecord) record = NULL;
  g_autoptr (BroBroError) e = NULL;
  int moved = 0;
  gboolean whole;
  for (guint i = 0; i < intent->items->len; i++)
    {
      BroCommitItem *item = intent->items->pdata[i];
      if (item->moved)
        moved++;
      else
        g_ptr_array_add (pending, item);
    }
  g_ptr_array_sort (pending, by_seq);
  whole = !intent->merge && pending->len > 0 && is_folder (self, intent->staging) && !bro_file_system_exists (self->fs, intent->destination);
  if (whole)
    {
      g_autofree char *parent = parent_of (intent->destination);
      if (!ensure_folders (self, root, parent, error) || !bro_file_system_rename (self->fs, intent->staging, intent->destination, error))
        return NULL;
    }
  for (guint i = 0; i < pending->len; i++)
    {
      BroCommitItem *item = pending->pdata[i];
      g_autofree char *point = NULL;
      if (!whole && !move_item (self, root, intent, item, error))
        return NULL;
      if (!bro_unit_repository_mark_moved (self->units, intent->unit_id, item->seq, error))
        return NULL;
      point = g_strdup_printf ("commit.afterMove:%d", ++moved);
      if (!crash_point (self, point, error))
        return NULL;
    }
  if (bro_file_system_exists (self->fs, staging_sums))
    {
      if (!ensure_folders (self, root, intent->destination, error))
        return NULL;
      if (intent->merge || bro_file_system_exists (self->fs, sums))
        {
          g_autoptr (GBytes) existing = bro_file_system_exists (self->fs, sums) ? bro_file_system_read (self->fs, sums, error) : g_bytes_new ("", 0);
          g_autoptr (GBytes) staged = existing ? bro_file_system_read (self->fs, staging_sums, error) : NULL;
          g_autofree char *old_text = NULL;
          g_autofree char *new_text = NULL;
          g_autofree char *merged = NULL;
          g_autoptr (GPtrArray) entries = NULL;
          g_autoptr (GBytes) bytes = NULL;
          if (!existing || !staged)
            return NULL;
          old_text = text_of (existing);
          new_text = text_of (staged);
          entries = bro_sha256_sums_parse (new_text);
          merged = bro_sha256_sums_merge (old_text, entries);
          bytes = g_bytes_new (merged, strlen (merged));
          if (!bro_file_system_write_atomically (self->fs, sums, bytes, FILE_MODE, error) || !bro_file_system_remove (self->fs, staging_sums, error))
            return NULL;
        }
      else if (!bro_file_system_rename (self->fs, staging_sums, sums, error))
        return NULL;
    }
  if (!remove_empty (self, intent->staging, error))
    return NULL;
  unit_files = files ? copy_files (files) : from_sums (self, intent, error);
  if (!unit_files)
    return NULL;
  folders = synced (root, intent, unit_files);
  for (guint i = 0; i < folders->len; i++)
    if (!bro_file_system_sync_directory (self->fs, folders->pdata[i], error))
      return NULL;
  if (!crash_point (self, "commit.beforeDone", error))
    return NULL;
  record = bro_unit_repository_get (self->units, intent->unit_id, &e);
  if (!record)
    {
      if (e)
        *error = g_steal_pointer (&e);
      else
        fail (error, bro_bro_message_new (BRO_MSG_UNIT_NOT_FOUND, NULL, BRO_SEVERITY_ERROR));
      return NULL;
    }
  record->state = intent->quarantine ? BRO_UNIT_STATE_QUARANTINED : BRO_UNIT_STATE_COMMITTED;
  record->has_committed_at = TRUE;
  record->committed_at = bro_engine_clock_now (self->clock);
  if (!bro_unit_repository_finish_commit (self->units, intent, record, unit_files, error))
    return NULL;
  return bro_commit_result_new (intent->unit_id, intent->destination, g_steal_pointer (&unit_files), intent->merge);
}

static BroCommitResult *
run (BroLibraryService *self, const BroUnitRecord *given_unit, const BroCommitIntent *given_intent, GPtrArray *files, const char *tag,
     BroBroError **error)
{
  g_autofree char *name = NULL;
  g_autofree char *root = NULL;
  g_autoptr (BroUnitRecord) unit = NULL;
  g_autoptr (BroCommitIntent) intent = NULL;
  g_autoptr (GPtrArray) open = NULL;
  BroCommitResult *result = NULL;
  if (!online (self, given_unit->library_id, &name, &root, error))
    return NULL;
  if (!tag && !inside (root, given_intent->destination))
    {
      fail (error, bro_bro_message_new (BRO_MSG_LIBRARY_NOT_FOUND, NULL, BRO_SEVERITY_ERROR));
      return NULL;
    }
  if (!check_marker (self, given_unit->library_id, name, root, error) || !claim (self, given_unit->id, error))
    return NULL;
  open = bro_unit_repository_open_intents (self->units, error);
  if (!open)
    goto out;
  for (guint i = 0; i < open->len; i++)
    if (strcmp (((BroCommitIntent *) open->pdata[i])->unit_id.value, given_unit->id.value) == 0)
      {
        result = complete (self, root, open->pdata[i], files, error);
        goto out;
      }
  unit = bro_unit_record_copy (given_unit);
  intent = bro_commit_intent_copy (given_intent);
  if (tag)
    {
      g_autofree char *folder = g_strconcat (root, "/" QUARANTINE_FOLDER, NULL);
      g_autofree char *base = last_part (intent->destination);
      g_autofree char *wanted = g_strdup_printf ("%s [%s]", base, tag);
      g_autoptr (GPtrArray) entries = NULL;
      g_autoptr (GPtrArray) names = NULL;
      g_autofree char *chosen = NULL;
      if (!ensure_folders (self, root, folder, error) || !(entries = bro_file_system_list (self->fs, folder, error)))
        goto out;
      names = g_ptr_array_new ();
      for (guint i = 0; i < entries->len; i++)
        g_ptr_array_add (names, ((BroDirectoryEntry *) entries->pdata[i])->name);
      g_ptr_array_add (names, NULL);
      chosen = bro_conflict_namer_next (wanted, (const char *const *) names->pdata, TRUE);
      g_free (intent->destination);
      intent->destination = g_strconcat (folder, "/", chosen, NULL);
      intent->merge = FALSE;
      intent->quarantine = TRUE;
      g_free (unit->path);
      unit->path = g_strconcat (QUARANTINE_FOLDER "/", chosen, NULL);
    }
  if (!precheck (self, intent, error))
    goto out;
  unit->state = BRO_UNIT_STATE_COMMITTING;
  if (!bro_unit_repository_begin_commit (self->units, unit, intent, error) || !crash_point (self, "commit.afterIntent", error))
    goto out;
  result = complete (self, root, intent, files, error);
out:
  unclaim (self, given_unit->id);
  return result;
}

static BroCommitResult *
port_commit (BroLibraryPort *port, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, BroBroError **error)
{
  return run (BRO_LIBRARY_SERVICE (port), unit, intent, files, NULL, error);
}

static BroCommitResult *
port_quarantine (BroLibraryPort *port, const BroUnitRecord *unit, const BroCommitIntent *intent, GPtrArray *files, const char *tag,
                 BroBroError **error)
{
  return run (BRO_LIBRARY_SERVICE (port), unit, intent, files, tag, error);
}

static void
library_port_iface_init (BroLibraryPortInterface *iface)
{
  iface->require = port_require;
  iface->staging_directory = port_staging_directory;
  iface->commit = port_commit;
  iface->quarantine = port_quarantine;
}

/* ---- roll forward ---- */

/* The configured library, read on the executor (from the configuration itself, so a roll forward works before
 * start), while the caller's worker waits. */
typedef struct {
  BroLibraryService *self;
  const char *library_id;
  char *name;
  char *path;
  gboolean done;
  GMutex lock;
  GCond cond;
} Configured;

static void
configured_action (gpointer data)
{
  Configured *c = data;
  g_autoptr (BroConfig) config = bro_config_source_current (c->self->config);
  g_autoptr (GPtrArray) libraries = bro_config_libraries (config);
  char *name = NULL;
  char *path = NULL;
  for (guint i = 0; i < libraries->len; i++)
    {
      BroLibraryEntry *w = libraries->pdata[i];
      if (strcmp (w->id, c->library_id) == 0)
        {
          name = g_strdup (*w->name ? w->name : w->id);
          path = g_strdup (w->path);
        }
    }
  g_mutex_lock (&c->lock);
  c->name = name;
  c->path = path;
  c->done = TRUE;
  g_cond_signal (&c->cond);
  g_mutex_unlock (&c->lock);
}

static gboolean
configured (BroLibraryService *self, const char *library_id, char **name, char **path, BroBroError **error)
{
  Configured c = { self, library_id, NULL, NULL, FALSE };
  g_mutex_init (&c.lock);
  g_cond_init (&c.cond);
  if (bro_engine_executor_post (self->executor, configured_action, &c, NULL))
    {
      g_mutex_lock (&c.lock);
      while (!c.done)
        g_cond_wait (&c.cond, &c.lock);
      g_mutex_unlock (&c.lock);
    }
  g_mutex_clear (&c.lock);
  g_cond_clear (&c.cond);
  if (!c.path)
    return fail (error, bro_bro_message_new (BRO_MSG_LIBRARY_NOT_FOUND, NULL, BRO_SEVERITY_ERROR));
  *name = c.name;
  *path = c.path;
  return TRUE;
}

BroCommitResult *
bro_library_service_roll_forward (BroLibraryService *self, const BroCommitIntent *intent, BroBroError **error)
{
  g_autoptr (BroBroError) e = NULL;
  g_autoptr (BroUnitRecord) unit = bro_unit_repository_get (self->units, intent->unit_id, &e);
  g_autofree char *name = NULL;
  g_autofree char *root = NULL;
  BroCommitResult *result;
  if (!unit)
    {
      if (e)
        *error = g_steal_pointer (&e);
      else
        fail (error, bro_bro_message_new (BRO_MSG_UNIT_NOT_FOUND, NULL, BRO_SEVERITY_ERROR));
      return NULL;
    }
  if (!configured (self, unit->library_id, &name, &root, error) || !check_marker (self, unit->library_id, name, root, error)
      || !claim (self, intent->unit_id, error))
    return NULL;
  result = complete (self, root, intent, NULL, error);
  unclaim (self, intent->unit_id);
  return result;
}
