/* bro-name-step.c */
#include "bro-name-step.h"
#include "bro-acquire-output.h"
#include "bro-archive-files.h"
#include "bro-conflict-namer.h"
#include "bro-directory-entry.h"
#include "bro-drive-entry.h"
#include "bro-file-info.h"
#include "bro-format-detector.h"
#include "bro-layouts.h"
#include "bro-library-handle.h"
#include "bro-name-output.h"
#include "bro-naming-settings.h"
#include "bro-plan-output.h"
#include "bro-planned-output.h"
#include "bro-planned-path.h"
#include "bro-probe-output.h"
#include "bro-step-sources.h"
#include "bro-steps-private.h"
#include "bro-template-engine.h"
#include "bro-token-registry.h"

#include <string.h>

struct _BroNameStep {
  GObject parent;
};

static void step_iface_init (BroStepInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (BroNameStep, bro_name_step, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_STEP, step_iface_init))

static BroStepKind
step_kind (BroStep *step)
{
  return BRO_STEP_KIND_NAME;
}

static BroResumability
step_resumability (BroStep *step)
{
  return BRO_RESUMABILITY_IDEMPOTENT;
}

static BroResourceSet *
step_resources (BroStep *step, const BroJobSnapshot *job)
{
  BroResourceSet *set = bro_resource_set_new ();
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (job);
  const char *writes[] = { plan && plan->library_id && *plan->library_id ? plan->library_id : NULL, NULL };
  set->library_writes = g_strdupv ((char **) writes);
  return set;
}

/* ---- failures ---- */

/* job.cancelled once the job is cancelled. */
static gboolean
cancelled (const BroStepContext *c, BroBroError **error)
{
  if (!bro_cancellation_token_is_cancelled (c->cancel))
    return FALSE;
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
  return TRUE;
}

/* @code with @params (taken; NULL: none). */
static BroBroError *
failure (BroMessageCode code, BroJsonValue *params)
{
  g_autoptr (BroBroMessage) m = bro_bro_message_new (code, params, BRO_SEVERITY_ERROR);
  return bro_bro_message_to_error (m, NULL);
}

/* ---- paths ---- */

static char *
parent_of (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash ? g_strndup (path, slash - path) : g_strdup ("");
}

static const char *
last_of (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash ? slash + 1 : path;
}

static char *
join (const char *a, const char *b)
{
  return !*a ? g_strdup (b) : !*b ? g_strdup (a) : g_strconcat (a, "/", b, NULL);
}

/* From the last '.' after the first character of the last component; "" when there is none. */
static const char *
extension_of (const char *path)
{
  const char *name = last_of (path);
  const char *dot = strrchr (name, '.');
  return dot && dot > name ? dot : "";
}

static char *
stem_of (const char *path)
{
  const char *name = last_of (path);
  return g_strndup (name, strlen (name) - strlen (extension_of (path)));
}

/* Upper-case ASCII letters lowered, as ConflictNamer compares names. */
static gboolean
same_ignoring_ascii_case (const char *a, const char *b)
{
  return g_ascii_strcasecmp (a, b) == 0;
}

/* ConflictNamer.next over @taken (char *). */
static char *
next_name (const char *name, GPtrArray *taken, gboolean is_folder)
{
  g_autoptr (GPtrArray) v = g_ptr_array_new ();
  for (guint i = 0; i < taken->len; i++)
    g_ptr_array_add (v, taken->pdata[i]);
  g_ptr_array_add (v, NULL);
  return bro_conflict_namer_next (name, (const char *const *) v->pdata, is_folder);
}

/* Adds the names in @folder (none when it isn't there) to @names (char *, g_free). */
static gboolean
add_names (BroFileSystem *fs, const char *folder, GPtrArray *names, BroBroError **error)
{
  g_autoptr (BroFileInfo) info = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  if (!bro_file_system_exists (fs, folder))
    return TRUE;
  if (!(info = bro_file_system_stat (fs, folder, error)))
    return FALSE;
  if (!info->is_directory)
    return TRUE;
  if (!(entries = bro_file_system_list (fs, folder, error)))
    return FALSE;
  for (guint i = 0; i < entries->len; i++)
    g_ptr_array_add (names, g_strdup (((BroDirectoryEntry *) entries->pdata[i])->name));
  return TRUE;
}

/* Whether @relative is taken under @root, ignoring ASCII case as ConflictNamer does: *@taken. */
static gboolean
is_taken (BroFileSystem *fs, const char *root, const char *relative, gboolean *taken, BroBroError **error)
{
  g_autofree char *parent = parent_of (relative);
  g_autofree char *folder = join (root, parent);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *next = NULL;
  if (!add_names (fs, folder, names, error))
    return FALSE;
  next = next_name (last_of (relative), names, FALSE);
  *taken = strcmp (next, last_of (relative)) != 0;
  return TRUE;
}

/* Adds what is in the way of item @i's name in @dir of staging to @taken (char *, g_free): the entries there but the item's
 * own location, and the other items' Acquire names (a later attempt looks for an item there first). */
static gboolean
add_in_staging (BroFileSystem *fs, const char *staging, GPtrArray *items, GPtrArray *at, guint i, const char *dir, GPtrArray *taken,
                BroBroError **error)
{
  const char *location = at->pdata[i];
  g_autofree char *at_parent = parent_of (location);
  g_autofree char *in_staging = join (staging, dir);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  if (!add_names (fs, in_staging, names, error))
    return FALSE;
  /* The item's own location isn't in the way of its new name. */
  for (guint k = 0; k < names->len; k++)
    if (!(strcmp (at_parent, dir) == 0 && strcmp (names->pdata[k], last_of (location)) == 0))
      g_ptr_array_add (taken, g_strdup (names->pdata[k]));
  for (guint j = 0; j < items->len; j++)
    {
      const char *from = ((const BroNamedFile *) items->pdata[j])->path;
      g_autofree char *from_parent = parent_of (from);
      if (j != i && strcmp (from_parent, dir) == 0)
        g_ptr_array_add (taken, g_strdup (last_of (from)));
    }
  return TRUE;
}

/* The first component of @path. */
static char *
first_of (const char *path)
{
  const char *slash = strchr (path, '/');
  return slash ? g_strndup (path, slash - path) : g_strdup (path);
}

/* Whether @path lies inside the folder @folder, ignoring ASCII case. */
static gboolean
inside (const char *path, const char *folder)
{
  size_t n = strlen (folder);
  return g_ascii_strncasecmp (path, folder, n) == 0 && path[n] == '/';
}

static gboolean
is_directory (BroFileSystem *fs, const char *path, gboolean *result, BroBroError **error)
{
  g_autoptr (BroFileInfo) info = bro_file_system_stat (fs, path, error);
  if (!info)
    return FALSE;
  *result = info->is_directory;
  return TRUE;
}

/* ---- what Name reads ---- */

/* The format a folder backup's structure shows (VIDEO_TS: DVD; BDMV/index.bdmv: UHD or Blu-ray), or
 * BRO_DISC_FORMAT_UNKNOWN. */
static gboolean
backup_format (BroFileSystem *fs, const char *backup, BroDiscFormat *format, BroBroError **error)
{
  g_autofree char *index = g_strconcat (backup, "/BDMV/index.bdmv", NULL);
  g_autofree char *video_ts = g_strconcat (backup, "/VIDEO_TS", NULL);
  g_autofree char *text = NULL;
  if (bro_file_system_exists (fs, index))
    {
      g_autoptr (GBytes) bytes = bro_file_system_read_range (fs, index, 0, 8, error);
      const guint8 *data;
      gsize n;
      if (!bytes)
        return FALSE;
      data = g_bytes_get_data (bytes, &n);
      text = g_malloc0 (n + 1);
      for (gsize i = 0; i < n; i++) /* as ASCII: a byte above 127 is '?' */
        text[i] = data[i] < 128 ? (char) data[i] : '?';
    }
  *format = bro_format_detector_detect (NULL, NULL, text, bro_file_system_exists (fs, video_ts));
  return TRUE;
}

/* The tokens shared by every file (bro_token_registry_values), or NULL and @error. */
static GHashTable *
token_values (const BroStepContext *c, const BroPlanOutput *plan, const BroIdentity *identity, const BroListing *listing, BroBroError **error)
{
  g_autoptr (BroDiscRequest) request = bro_step_sources_request (c->job, error);
  g_autoptr (BroDriveLocation) drive = NULL;
  g_autoptr (GPtrArray) entries = bro_config_drives (c->config);
  g_autofree char *label = NULL;
  const char *drive_name = "";
  char job[9] = { 0 };
  BroTokenContext t = { 0 };
  if (!request)
    return NULL;
  if (request->source->kind == BRO_DISC_SOURCE_DRIVE)
    drive = bro_drive_lookup_find (c->drive_lookup, request->source->value);
  for (guint i = 0; drive && drive->config_id && i < entries->len; i++)
    {
      const BroDriveEntry *e = entries->pdata[i];
      if (g_strcmp0 (e->id, drive->config_id) == 0)
        {
          drive_name = e->name ? e->name : "";
          break;
        }
    }
  label = bro_step_sources_label (request->source, drive);
  for (const char *p = c->job->id.value; *p && strlen (job) < 8; p++)
    if (*p != '-')
      job[strlen (job)] = *p;
  t.rip = plan->mode == BRO_RIP_MODE_MKV || plan->mode == BRO_RIP_MODE_BACKUP_THEN_MKV ? "Rip" : "Backup";
  t.drive = drive_name;
  t.disc = listing && listing->name && *listing->name ? listing->name : label;
  t.volume = listing && listing->volume_name && *listing->volume_name ? listing->volume_name : label;
  t.type = listing ? listing->type : BRO_DISC_TYPE_DISC;
  t.job = job;
  t.release_year = -1;
  t.has_local_time = TRUE;
  t.local_time = bro_clock_local_time (c->clock, c->job->has_started_at ? c->job->started_at : bro_clock_now (c->clock));
  return bro_token_registry_values (identity, &t);
}

static void
put (GHashTable *values, const char *key, char *value)
{
  g_hash_table_insert (values, g_strdup (key), value);
}

/* @name with ASCII white space (space, \t, \n, \v, \f, \r) cut from both ends. */
static char *
trimmed (const char *name)
{
  return g_strstrip (g_strdup (name ? name : ""));
}

/* Each item's wanted path in the unit's folder (Layouts.paths without the folder; char *), or NULL and name.unusable. */
static GPtrArray *
desired_paths (GPtrArray *items, const BroPlanOutput *plan, const BroAcquireOutput *acquired, const BroListing *listing,
               const BroNamingSettings *naming, GHashTable *values, const char *folder, BroBroError **error)
{
  g_autoptr (GPtrArray) titles = g_ptr_array_new (); /* const BroTitle *, nullable: the listing's of each Acquire title */
  g_autoptr (GPtrArray) outputs = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_planned_output_free);
  g_autoptr (GArray) output_of = g_array_new (FALSE, FALSE, sizeof (int)); /* per item: its output, -1: none */
  g_autoptr (GPtrArray) hand = g_ptr_array_new_with_free_func (g_free);    /* per item: a hand-given name, nullable */
  g_autoptr (GPtrArray) paths = NULL;
  g_autoptr (GPtrArray) result = g_ptr_array_new_with_free_func (g_free);
  int main = -1, longest = 0;
  gboolean has_main = FALSE;

  for (guint i = 0; i < acquired->titles->len; i++)
    {
      const BroAcquiredTitle *a = acquired->titles->pdata[i];
      const BroTitle *found = NULL;
      for (guint j = 0; listing && j < listing->titles->len && !found; j++)
        if (((const BroTitle *) listing->titles->pdata[j])->index == a->index)
          found = listing->titles->pdata[j];
      g_ptr_array_add (titles, (gpointer) found);
      /* The longest, the first on a tie. */
      if (found && (!has_main || found->duration_seconds > longest))
        {
          has_main = TRUE;
          longest = found->duration_seconds;
          main = found->index;
        }
    }

  for (guint i = 0; i < items->len; i++)
    {
      const BroNamedFile *item = items->pdata[i];
      const char *ext = extension_of (item->path);
      int none = -1;
      if (g_strcmp0 (item->role, "title") == 0 && item->has_title)
        {
          BroPlannedOutput *o = bro_planned_output_new (BRO_PATH_ROLE_TITLE);
          const BroTitle *t = NULL;
          const char *disc_name = listing && listing->name ? listing->name : "";
          int n = 0;
          for (guint j = 0; j < acquired->titles->len && n == 0; j++)
            if (((const BroAcquiredTitle *) acquired->titles->pdata[j])->index == item->title)
              n = (int) j + 1;
          t = n > 0 ? titles->pdata[n - 1] : NULL;
          put (o->values, "track", t ? bro_token_registry_track_label (t) : g_strdup (""));
          put (o->values, "title", g_strdup (t && t->name && *t->name ? t->name : disc_name));
          put (o->values, "index", g_strdup_printf ("%d", item->title));
          put (o->values, "n", g_strdup_printf ("%d", n));
          put (o->values, "source", t && t->has_source_title_id ? g_strdup_printf ("%d", t->source_title_id) : g_strdup (""));
          put (o->values, "duration",
               t ? g_strdup_printf ("%d-%02d-%02d", t->duration_seconds / 3600, t->duration_seconds / 60 % 60, t->duration_seconds % 60)
                 : g_strdup (""));
          put (o->values, "chapters", t ? g_strdup_printf ("%d", t->chapters) : g_strdup (""));
          put (o->values, "original", stem_of (item->path));
          put (o->values, "comment", g_strdup (t && t->comment ? t->comment : ""));
          o->title = item->title;
          o->main_feature = has_main && item->title == main;
          g_free (o->extension);
          o->extension = g_strdup (ext);
          {
            g_autofree char *name = NULL;
            char *file = NULL;
            for (guint j = 0; j < plan->titles->len && !name; j++)
              if (((const BroTitleChoice *) plan->titles->pdata[j])->index == item->title)
                name = trimmed (((const BroTitleChoice *) plan->titles->pdata[j])->name);
            if (name && *name)
              {
                g_autoptr (GHashTable) all = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
                GHashTableIter it;
                gpointer k, v;
                gsize len;
                g_hash_table_iter_init (&it, values);
                while (g_hash_table_iter_next (&it, &k, &v))
                  put (all, k, g_strdup (v));
                g_hash_table_iter_init (&it, o->values);
                while (g_hash_table_iter_next (&it, &k, &v))
                  put (all, k, g_strdup (v));
                file = bro_template_engine_render_path (name, all);
                len = strlen (file);
                if (len > 0 && !(len >= 4 && g_ascii_strcasecmp (file + len - 4, ".mkv") == 0))
                  {
                    char *with = g_strconcat (file, ".mkv", NULL);
                    g_free (file);
                    file = with;
                  }
              }
            g_ptr_array_add (hand, file);
          }
          g_array_append_val (output_of, outputs->len);
          g_ptr_array_add (outputs, o);
        }
      else if (g_strcmp0 (item->role, "backup") == 0)
        {
          BroPlannedOutput *o = bro_planned_output_new (BRO_PATH_ROLE_BACKUP);
          const char *keys[] = { "title", "index", "n", "source", "duration", "chapters", "original", "comment" };
          for (guint k = 0; k < G_N_ELEMENTS (keys); k++)
            put (o->values, keys[k], g_strdup (""));
          put (o->values, "rip", g_strdup ("Backup"));
          g_free (o->extension);
          o->extension = g_strdup (ext);
          g_ptr_array_add (hand, NULL);
          g_array_append_val (output_of, outputs->len);
          g_ptr_array_add (outputs, o);
        }
      else
        {
          g_ptr_array_add (hand, NULL);
          g_array_append_val (output_of, none);
        }
    }

  paths = bro_layouts_paths (naming, values, outputs);
  for (guint i = 0; i < items->len; i++)
    {
      const BroNamedFile *item = items->pdata[i];
      int o = g_array_index (output_of, int, i);
      g_auto (GStrv) parts = NULL;
      gboolean unusable;
      char *rel;
      if (o < 0)
        rel = g_strdup (item->path);
      else if (hand->pdata[i])
        rel = g_strdup (hand->pdata[i]);
      else
        {
          const char *path = ((const BroPlannedPath *) paths->pdata[o])->path;
          const char *rest = strlen (path) > strlen (folder) ? path + strlen (folder) : "";
          while (*rest == '/')
            rest++;
          rel = g_strdup (rest);
        }
      g_ptr_array_add (result, rel);
      parts = g_strsplit (rel, "/", -1);
      unusable = !*rel; /* g_strsplit gives no part for "" */
      for (guint p = 0; parts[p] && !unusable; p++)
        unusable = !*parts[p] || parts[p][0] == '.';
      if (unusable || (!strchr (rel, '/') && bro_archive_files_is_own_file (rel)))
        {
          g_autofree char *path = g_strconcat (folder, "/", rel, NULL);
          *error = failure (BRO_MSG_NAME_UNUSABLE, _bro_param ("path", bro_json_value_new_string (path)));
          return NULL;
        }
    }
  return g_steal_pointer (&result);
}

/* ---- the step ---- */

/* Renames @from to @to in staging, making the folders it goes in; a cancel comes first. */
static gboolean
rename_in (const BroStepContext *c, const char *staging, const char *from, const char *to, BroBroError **error)
{
  g_autofree char *parent = parent_of (to);
  g_auto (GStrv) parts = g_strsplit (parent, "/", -1);
  g_autofree char *made = g_strdup (staging);
  g_autofree char *from_path = g_strconcat (staging, "/", from, NULL);
  g_autofree char *to_path = g_strconcat (staging, "/", to, NULL);
  BroJsonValue *params;
  if (cancelled (c, error))
    return FALSE;
  for (guint p = 0; parts[p]; p++)
    {
      char *next;
      if (!*parts[p])
        continue;
      next = g_strconcat (made, "/", parts[p], NULL);
      g_free (made);
      made = next;
      if (!bro_file_system_exists (c->fs, made) && !bro_file_system_create_directory (c->fs, made, TRUE, error))
        return FALSE;
    }
  if (!bro_file_system_rename (c->fs, from_path, to_path, error))
    return FALSE;
  params = _bro_param ("from", bro_json_value_new_string (from));
  bro_json_value_set (params, "to", bro_json_value_new_string (to));
  _bro_step_log (c, BRO_MSG_RIP_RENAMED, params, BRO_SEVERITY_INFO);
  return TRUE;
}

static BroJsonValue *
checkpoint_of (const char *folder, GPtrArray *items, GPtrArray *at, GPtrArray *to, GPtrArray *via)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *files = bro_json_value_new_array ();
  for (guint i = 0; i < items->len; i++)
    {
      BroJsonValue *f = bro_json_value_new_object ();
      bro_json_value_set (f, "from", bro_json_value_new_string (((const BroNamedFile *) items->pdata[i])->path));
      bro_json_value_set (f, "at", bro_json_value_new_string (at->pdata[i]));
      bro_json_value_set (f, "to", bro_json_value_new_string (to->pdata[i]));
      if (via->pdata[i])
        bro_json_value_set (f, "via", bro_json_value_new_string (via->pdata[i]));
      bro_json_value_append (files, f);
    }
  bro_json_value_set (o, "kind", bro_json_value_new_string (BRO_NAME_STEP_NAMES_CHECKPOINT));
  bro_json_value_set (o, "folder", bro_json_value_new_string (folder));
  bro_json_value_set (o, "files", files);
  return o;
}

static gboolean
same_json (const BroJsonValue *a, const BroJsonValue *b)
{
  g_autoptr (GBytes) x = bro_json_value_encode_canonical (a);
  g_autoptr (GBytes) y = bro_json_value_encode_canonical (b);
  return g_bytes_equal (x, y);
}

static BroStepReport *
step_run (BroStep *step, const BroStepContext *context, BroBroError **error)
{
  const BroStepContext *c = context;
  BroFileSystem *fs = c->fs;
  g_autoptr (BroPlanOutput) plan = bro_plan_output_of (c->job);
  g_autoptr (BroAcquireOutput) acquired = bro_acquire_output_of (c->job);
  g_autoptr (BroLibraryHandle) handle = NULL;
  g_autofree char *staging = NULL;
  g_autoptr (BroNamingSettings) naming = NULL;
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  g_autoptr (BroJsonValue) saved = NULL;
  g_autoptr (GHashTable) saved_files = g_hash_table_new (g_str_hash, g_str_equal); /* from → BroJsonValue * (in saved) */
  g_autoptr (GPtrArray) items = NULL;
  g_autoptr (GPtrArray) at = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) to = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) via = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) root = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) desired = NULL;
  g_autoptr (BroIdentity) corrected = NULL;
  g_autoptr (BroProbeOutput) probe = NULL;
  g_autoptr (GHashTable) values = NULL;
  g_autoptr (BroJsonValue) checkpoint = NULL;
  g_autoptr (BroNameOutput) output = NULL;
  g_autofree char *wanted = NULL;
  g_autofree char *folder = NULL;
  g_autofree char *in_library = NULL;
  const BroIdentity *identity;
  const BroListing *listing;
  BroJsonValue *naming_json, *probe_json, *files;
  const char *library;
  gboolean merge;
  int backup = -1;

  if (!plan || !acquired)
    return _bro_step_skipped ();
  if (!(handle = bro_library_port_require (c->library, plan->library_id, error)))
    return NULL;
  library = handle->path;
  if (!(staging = bro_library_port_staging_directory (c->library, plan->library_id, &c->job->id, error)))
    return NULL;
  naming_json = bro_json_value_member (plan->profile, "naming");
  naming = bro_naming_settings_decode (naming_json ? naming_json : empty);
  if (plan->mode != BRO_RIP_MODE_BACKUP_THEN_MKV)
    {
      g_free (naming->backup_subfolder);
      naming->backup_subfolder = g_strdup ("");
    }
  saved = bro_checkpointer_saved (c->checkpoint);
  if (saved
      && (saved->kind != BRO_JSON_VALUE_OBJECT
          || g_strcmp0 (bro_json_value_get_string (bro_json_value_member (saved, "kind"), NULL), BRO_NAME_STEP_NAMES_CHECKPOINT) != 0))
    g_clear_pointer (&saved, bro_json_value_unref);
  items = bro_name_output_from_acquire (acquired);
  /* A checkpoint made for other items than Acquire's is no guide: Acquire ran again since (a retry from it or Plan), and
   * removed whatever Name had renamed, so every item is under Acquire's name. */
  if (saved)
    {
      g_autoptr (GHashTable) was = g_hash_table_new (g_str_hash, g_str_equal);
      gboolean same = TRUE;
      files = bro_json_value_member (saved, "files");
      for (guint i = 0; i < bro_json_value_length (files); i++)
        g_hash_table_add (was, (gpointer) bro_json_value_get_string (bro_json_value_member (bro_json_value_at (files, i), "from"), ""));
      for (guint i = 0; i < items->len && same; i++)
        same = g_hash_table_contains (was, ((const BroNamedFile *) items->pdata[i])->path);
      for (guint i = 0; i < bro_json_value_length (files) && same; i++)
        {
          const char *from = bro_json_value_get_string (bro_json_value_member (bro_json_value_at (files, i), "from"), "");
          gboolean found = FALSE;
          for (guint k = 0; k < items->len && !found; k++)
            found = strcmp (((const BroNamedFile *) items->pdata[k])->path, from) == 0;
          same = found;
        }
      if (!same)
        g_clear_pointer (&saved, bro_json_value_unref);
    }
  files = saved ? bro_json_value_member (saved, "files") : NULL;
  for (guint i = 0; i < bro_json_value_length (files); i++)
    {
      BroJsonValue *f = bro_json_value_at (files, i);
      const char *from = bro_json_value_get_string (bro_json_value_member (f, "from"), NULL);
      if (from && !g_hash_table_contains (saved_files, from))
        g_hash_table_insert (saved_files, (gpointer) from, f);
    }

  /* 1. Where each produced item is now: where an earlier attempt sent it (its target, or the free name it went to on the
   * way), else where that attempt found it, else under Acquire's name. In that order: a folder made for a target may be
   * where an item was (ASCII case aside), but nothing but the item is ever at its target or its way. */
  for (guint i = 0; i < items->len; i++)
    {
      const BroNamedFile *item = items->pdata[i];
      BroJsonValue *was = g_hash_table_lookup (saved_files, item->path);
      const char *candidates[] = { bro_json_value_get_string (bro_json_value_member (was, "to"), NULL),
                                   bro_json_value_get_string (bro_json_value_member (was, "via"), NULL),
                                   bro_json_value_get_string (bro_json_value_member (was, "at"), NULL), item->path };
      const char *found = NULL;
      for (guint k = 0; k < G_N_ELEMENTS (candidates) && !found; k++)
        if (candidates[k] && *candidates[k])
          {
            g_autofree char *path = g_strconcat (staging, "/", candidates[k], NULL);
            if (bro_file_system_exists (fs, path))
              found = candidates[k];
          }
      if (!found)
        {
          *error = failure (BRO_MSG_ARCHIVE_FILE_MISSING, _bro_param ("file", bro_json_value_new_string (item->path)));
          return NULL;
        }
      g_ptr_array_add (at, g_strdup (found));
      if (backup < 0 && g_strcmp0 (item->role, "backup") == 0)
        backup = (int) i;
    }

  /* 2. The format a folder backup shows. */
  identity = plan->identity;
  if (backup >= 0)
    {
      g_autofree char *path = g_strconcat (staging, "/", (char *) at->pdata[backup], NULL);
      gboolean folder_backup = FALSE;
      BroDiscFormat format = BRO_DISC_FORMAT_UNKNOWN;
      if (!is_directory (fs, path, &folder_backup, error))
        return NULL;
      if (folder_backup && !backup_format (fs, path, &format, error))
        return NULL;
      if (folder_backup && format != BRO_DISC_FORMAT_UNKNOWN && format != identity->format)
        {
          g_autoptr (BroJsonValue) json = bro_identity_encode (identity);
          corrected = bro_identity_decode (json);
          corrected->format = format;
          corrected->format_code = bro_format_detector_code (format, corrected->encrypted);
          identity = corrected;
          _bro_step_log (c, BRO_MSG_NAME_FORMAT_FROM_BACKUP, _bro_param ("format", bro_json_value_new_string (identity->format_code.text)),
                         BRO_SEVERITY_INFO);
        }
    }
  probe_json = bro_job_snapshot_output (c->job, BRO_STEP_KIND_PROBE);
  probe = probe_json && probe_json->kind == BRO_JSON_VALUE_OBJECT ? bro_probe_output_decode (probe_json) : NULL;
  listing = probe ? probe->listing : NULL;
  if (!(values = token_values (c, plan, identity, listing, error)))
    return NULL;

  /* 3. The unit's folder. */
  wanted = bro_layouts_folder (naming, values);
  if (!*wanted)
    {
      *error = failure (BRO_MSG_NAME_NO_FOLDER, NULL);
      return NULL;
    }
  {
    const char *slash = strchr (wanted, '/');
    g_autofree char *first = slash ? g_strndup (wanted, slash - wanted) : g_strdup (wanted);
    if (same_ignoring_ascii_case (first, "_quarantine"))
      {
        *error = failure (BRO_MSG_NAME_UNUSABLE, _bro_param ("path", bro_json_value_new_string (wanted)));
        return NULL;
      }
  }
  merge = naming->layout == BRO_LAYOUT_MEDIA_SERVER || naming->conflict_policy == BRO_CONFLICT_POLICY_MERGE;
  folder = g_strdup (wanted);
  if (!merge)
    {
      const char *kept = saved ? bro_json_value_get_string (bro_json_value_member (saved, "folder"), NULL) : NULL;
      gboolean kept_taken = TRUE, wanted_taken = FALSE;
      if (kept && *kept && !is_taken (fs, library, kept, &kept_taken, error))
        return NULL;
      if (kept && *kept && !kept_taken && (naming->conflict_policy != BRO_CONFLICT_POLICY_SKIP || strcmp (kept, wanted) == 0))
        {
          g_free (folder);
          folder = g_strdup (kept);
        }
      else
        {
          if (!is_taken (fs, library, wanted, &wanted_taken, error))
            return NULL;
          if (wanted_taken)
            {
              g_autofree char *parent = parent_of (wanted);
              g_autofree char *where = join (library, parent);
              g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
              g_autofree char *free_name = NULL;
              if (naming->conflict_policy == BRO_CONFLICT_POLICY_SKIP)
                {
                  g_autofree char *path = g_strconcat (library, "/", wanted, NULL);
                  *error = failure (BRO_MSG_LIBRARY_FOLDER_EXISTS, _bro_param ("path", bro_json_value_new_string (path)));
                  return NULL;
                }
              if (!add_names (fs, where, names, error))
                return NULL;
              free_name = next_name (last_of (wanted), names, TRUE);
              g_free (folder);
              folder = join (parent, free_name);
            }
        }
    }
  in_library = g_strconcat (library, "/", folder, NULL);

  /* 4. Every item's name in it: a name kept from an earlier attempt, else a free one: never a name another entry in
   * staging holds, nor another item's Acquire name (where a retry from Acquire puts that item again). */
  if (!(desired = desired_paths (items, plan, acquired, listing, naming, values, wanted, error)))
    return NULL;
  g_ptr_array_set_size (to, items->len);
  for (guint i = 0; i < items->len; i++)
    {
      BroJsonValue *was = g_hash_table_lookup (saved_files, ((const BroNamedFile *) items->pdata[i])->path);
      const char *kept = bro_json_value_get_string (bro_json_value_member (was, "to"), NULL);
      gboolean taken = FALSE;
      if (!kept || !*kept)
        continue;
      if (merge && !is_taken (fs, in_library, kept, &taken, error))
        return NULL;
      if (!taken)
        to->pdata[i] = g_strdup (kept);
    }
  for (guint i = 0; i < items->len; i++)
    {
      const char *location = at->pdata[i];
      g_autofree char *dir = NULL;
      g_autofree char *at_path = g_strconcat (staging, "/", location, NULL);
      g_autoptr (GPtrArray) taken = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *free_name = NULL;
      char *target;
      gboolean item_is_folder = FALSE;
      if (to->pdata[i])
        continue;
      dir = parent_of (desired->pdata[i]);
      if (!add_in_staging (fs, staging, items, at, i, dir, taken, error))
        return NULL;
      for (guint k = 0; k < to->len; k++)
        if (to->pdata[k])
          {
            g_autofree char *p = parent_of (to->pdata[k]);
            if (strcmp (p, dir) == 0)
              g_ptr_array_add (taken, g_strdup (last_of (to->pdata[k])));
          }
      if (merge)
        {
          g_autofree char *shared = join (in_library, dir);
          if (!add_names (fs, shared, taken, error))
            return NULL;
        }
      if (!is_directory (fs, at_path, &item_is_folder, error))
        return NULL;
      free_name = next_name (last_of (desired->pdata[i]), taken, item_is_folder);
      target = join (dir, free_name);
      /* A case-insensitive disk can't rename to a case alone. */
      if (same_ignoring_ascii_case (target, location))
        {
          g_free (target);
          target = g_strdup (location);
        }
      to->pdata[i] = target;
    }
  /* No item goes inside another item's target. An item whose place holds a target (its own too, ASCII case ignored: a disk
   * may not tell "backup" from "Backup/") first goes to a free name at staging's root, so nothing moves into a folder that
   * moves, nor into itself. */
  for (guint i = 0; i < items->len; i++)
    for (guint j = 0; j < items->len; j++)
      if (j != i && inside (to->pdata[i], to->pdata[j]))
        {
          g_autofree char *path = g_strconcat (folder, "/", (char *) to->pdata[i], NULL);
          *error = failure (BRO_MSG_NAME_UNUSABLE, _bro_param ("path", bro_json_value_new_string (path)));
          return NULL;
        }

  g_ptr_array_set_size (via, items->len);
  if (!add_names (fs, staging, root, error))
    return NULL;
  for (guint j = 0; j < items->len; j++)
    {
      g_ptr_array_add (root, first_of (((const BroNamedFile *) items->pdata[j])->path));
      g_ptr_array_add (root, first_of (at->pdata[j]));
      g_ptr_array_add (root, first_of (to->pdata[j]));
    }
  for (guint j = 0; j < items->len; j++)
    {
      gboolean holds = FALSE, is_folder = FALSE;
      g_autofree char *at_path = g_strconcat (staging, "/", (char *) at->pdata[j], NULL);
      if (strcmp (to->pdata[j], at->pdata[j]) == 0)
        continue;
      for (guint i = 0; i < items->len && !holds; i++)
        holds = inside (to->pdata[i], at->pdata[j]);
      if (!holds)
        continue;
      if (!is_directory (fs, at_path, &is_folder, error))
        return NULL;
      via->pdata[j] = next_name (last_of (at->pdata[j]), root, is_folder);
      g_ptr_array_add (root, g_strdup (via->pdata[j]));
    }

  /* 5. The decisions are kept before anything moves, then the renames. */
  checkpoint = checkpoint_of (folder, items, at, to, via);
  if (cancelled (c, error))
    return NULL;
  if ((!saved || !same_json (saved, checkpoint)) && !bro_checkpointer_save (c->checkpoint, checkpoint, error))
    return NULL;
  _bro_step_log (c, BRO_MSG_ARCHIVE_OUTPUT_FOLDER, _bro_param ("path", bro_json_value_new_string (in_library)), BRO_SEVERITY_INFO);
  for (guint i = 0; i < items->len; i++)
    if (via->pdata[i] && !rename_in (c, staging, at->pdata[i], via->pdata[i], error))
      return NULL;
  for (guint i = 0; i < items->len; i++)
    {
      const char *from = via->pdata[i] ? via->pdata[i] : at->pdata[i];
      if (strcmp (to->pdata[i], from) != 0 && !rename_in (c, staging, from, to->pdata[i], error))
        return NULL;
    }
  if (corrected && strcmp (to->pdata[backup], at->pdata[backup]) != 0)
    _bro_step_log (c, BRO_MSG_BACKUP_RENAMED, _bro_param ("name", bro_json_value_new_string (last_of (to->pdata[backup]))), BRO_SEVERITY_INFO);

  output = bro_name_output_new ();
  g_free (output->folder);
  output->folder = g_strdup (folder);
  g_free (output->name);
  output->name = g_strdup (last_of (folder));
  output->merge = merge;
  for (guint i = 0; i < items->len; i++)
    {
      BroNamedFile *f = bro_named_file_copy (items->pdata[i]);
      g_free (f->path);
      f->path = g_strdup (to->pdata[i]);
      g_ptr_array_add (output->files, f);
    }
  output->identity = g_steal_pointer (&corrected);
  return bro_step_report_new (bro_name_output_encode (output));
}

static void
step_iface_init (BroStepInterface *iface)
{
  iface->kind = step_kind;
  iface->resumability = step_resumability;
  iface->resources = step_resources;
  iface->run = step_run;
}

static void
bro_name_step_class_init (BroNameStepClass *klass)
{
}

static void
bro_name_step_init (BroNameStep *self)
{
}

BroNameStep *
bro_name_step_new (void)
{
  return g_object_new (BRO_TYPE_NAME_STEP, NULL);
}
