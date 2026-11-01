/* bro-scenario-world.c */
#include "bro-scenario-world.h"
#include "bro-scenario-catalog.h"
#include "bro-test-fixtures.h"

#include "bro-archive-record-codec.h"
#include "bro-commit-intent.h"
#include "bro-device-event.h"
#include "bro-fingerprint.h"
#include "bro-library-marker.h"
#include "bro-library-record.h"
#include "bro-listing-builder.h"
#include "bro-media-kind.h"
#include "bro-os-drive.h"
#include "bro-robot.h"
#include "bro-sha256-sums.h"
#include "bro-sum-entry.h"
#include "bro-unit-file.h"
#include "bro-unit-record.h"
#include "bro-unit-status.h"

#include <string.h>

#define DEFAULT_CONFIG "config3-scenario-base.json"
#define DEFAULT_MAKEMKVCON "tools/makemkvcon-saves.yaml"

#define J(v, k) bro_json_value_member ((v), (k))
#define S(v, k, d) bro_json_value_get_string (J ((v), (k)), (d))

char *
bro_scenario_failure (const char *expected, const char *found)
{
  return g_strconcat (expected, " / ", found, NULL);
}

static void
drive_free (gpointer data)
{
  BroScenarioDrive *d = data;
  g_free (d->name);
  g_free (d->device);
  g_free (d->identification);
  g_clear_pointer (&d->disc, bro_json_value_unref);
  g_free (d);
}

BroScenarioWorld *
bro_scenario_world_new (BroJsonValue *scenario)
{
  BroScenarioWorld *w = g_new0 (BroScenarioWorld, 1);
  BroJsonValue *given = J (scenario, "given"), *tools, *drives, *disc;
  g_mutex_init (&w->lock);
  w->given = given ? bro_json_value_ref (given) : bro_json_value_new_object ();
  w->drives = g_ptr_array_new_with_free_func (drive_free);
  w->scripts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) bro_json_value_unref);
  w->markers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  w->disc = (disc = J (w->given, "disc")) ? bro_scenario_catalog_disc (disc) : NULL;
  g_hash_table_insert (w->scripts, g_strdup ("makemkvcon"), bro_scenario_catalog_read (DEFAULT_MAKEMKVCON));
  tools = J (w->given, "tools");
  for (guint i = 0; tools && tools->kind == BRO_JSON_VALUE_OBJECT && i < tools->keys->len; i++)
    {
      BroJsonValue *script = tools->items->pdata[i];
      if (script->kind == BRO_JSON_VALUE_NULL)
        g_hash_table_remove (w->scripts, tools->keys->pdata[i]);
      else
        g_hash_table_replace (w->scripts, g_strdup (tools->keys->pdata[i]), bro_scenario_catalog_script (script));
    }
  drives = J (w->given, "drives");
  for (guint i = 0; i < bro_json_value_length (drives); i++)
    {
      BroJsonValue *d = bro_json_value_at (drives, i), *index = J (d, "makemkvIndex"), *driven = J (d, "disc");
      BroScenarioDrive *drive = g_new0 (BroScenarioDrive, 1);
      const char *id = S (d, "id", "");
      g_autofree char *dev = g_strconcat ("/dev/", id, NULL);
      g_autofree char *ident = g_strconcat ("BD-RE ", id, NULL);
      drive->name = g_strdup (id);
      drive->device = g_strdup (S (d, "device", dev));
      drive->identification = g_strdup (S (d, "identification", S (d, "name", ident)));
      drive->index = index && index->kind == BRO_JSON_VALUE_INTEGER ? (int) index->integer : (int) i;
      drive->tray_motor = bro_json_value_get_bool (J (d, "trayMotor"), TRUE);
      drive->disc = driven ? bro_scenario_catalog_disc (driven) : NULL;
      g_ptr_array_add (w->drives, drive);
    }
  return w;
}

void
bro_scenario_world_free (BroScenarioWorld *w)
{
  if (!w)
    return;
  bro_json_value_unref (w->given);
  g_ptr_array_unref (w->drives);
  g_hash_table_unref (w->scripts);
  g_hash_table_unref (w->markers);
  g_clear_pointer (&w->disc, bro_json_value_unref);
  g_mutex_clear (&w->lock);
  g_free (w);
}

gboolean
bro_scenario_world_installed (BroScenarioWorld *w, const char *tool)
{
  gboolean there;
  g_mutex_lock (&w->lock);
  there = g_hash_table_contains (w->scripts, tool);
  g_mutex_unlock (&w->lock);
  return there;
}

void
bro_scenario_world_set_script (BroScenarioWorld *w, const char *tool, BroJsonValue *script)
{
  BroJsonValue *read = script && script->kind != BRO_JSON_VALUE_NULL ? bro_scenario_catalog_script (script) : NULL;
  g_mutex_lock (&w->lock);
  if (read)
    g_hash_table_replace (w->scripts, g_strdup (tool), read);
  else
    g_hash_table_remove (w->scripts, tool);
  g_mutex_unlock (&w->lock);
}

BroJsonValue *
bro_scenario_world_script (BroScenarioWorld *w, const char *tool)
{
  BroJsonValue *s;
  g_mutex_lock (&w->lock);
  s = g_hash_table_lookup (w->scripts, tool);
  s = s ? bro_json_value_ref (s) : NULL;
  g_mutex_unlock (&w->lock);
  return s;
}

BroJsonValue *
bro_scenario_world_disc (BroScenarioWorld *w)
{
  BroJsonValue *d;
  g_mutex_lock (&w->lock);
  d = w->disc ? bro_json_value_ref (w->disc) : NULL;
  g_mutex_unlock (&w->lock);
  return d;
}

void
bro_scenario_world_set_disc (BroScenarioWorld *w, BroJsonValue *disc)
{
  g_mutex_lock (&w->lock);
  g_clear_pointer (&w->disc, bro_json_value_unref);
  w->disc = disc ? bro_json_value_ref (disc) : NULL;
  g_mutex_unlock (&w->lock);
}

BroScenarioDrive *
bro_scenario_world_drive_named (BroScenarioWorld *w, const char *name, char **failure)
{
  g_autofree char *what = NULL;
  for (guint i = 0; i < w->drives->len; i++)
    if (g_strcmp0 (((BroScenarioDrive *) w->drives->pdata[i])->name, name) == 0)
      return w->drives->pdata[i];
  what = g_strconcat ("a drive ", name, NULL);
  if (failure && !*failure)
    *failure = bro_scenario_failure (what, "none");
  return NULL;
}

/* ---- JSON helpers ---- */

BroJsonValue *
bro_scenario_merge_patch (BroJsonValue *target, BroJsonValue *patch)
{
  BroJsonValue *out;
  if (!patch || patch->kind != BRO_JSON_VALUE_OBJECT)
    return patch ? bro_json_value_ref (patch) : bro_json_value_new_null ();
  out = bro_json_value_new_object ();
  for (guint i = 0; target && target->kind == BRO_JSON_VALUE_OBJECT && i < target->keys->len; i++)
    {
      BroJsonValue *p = J (patch, target->keys->pdata[i]);
      if (!p)
        bro_json_value_set (out, target->keys->pdata[i], bro_json_value_ref (target->items->pdata[i]));
      else if (p->kind != BRO_JSON_VALUE_NULL)
        bro_json_value_set (out, target->keys->pdata[i], bro_scenario_merge_patch (target->items->pdata[i], p));
    }
  for (guint i = 0; i < patch->keys->len; i++)
    {
      BroJsonValue *p = patch->items->pdata[i];
      if (p->kind == BRO_JSON_VALUE_NULL || (target && J (target, patch->keys->pdata[i])))
        continue;
      bro_json_value_set (out, patch->keys->pdata[i], bro_scenario_merge_patch (NULL, p));
    }
  return out;
}

BroJsonValue *
bro_scenario_fill_root (BroJsonValue *v)
{
  BroJsonValue *c;
  switch (v->kind)
    {
    case BRO_JSON_VALUE_STRING:
      {
        g_autoptr (GString) s = g_string_new (v->string);
        g_string_replace (s, "{root}", TEST_ROOT, 0);
        return bro_json_value_new_string (s->str);
      }
    case BRO_JSON_VALUE_ARRAY:
      c = bro_json_value_new_array ();
      for (guint i = 0; i < v->items->len; i++)
        bro_json_value_append (c, bro_scenario_fill_root (v->items->pdata[i]));
      return c;
    case BRO_JSON_VALUE_OBJECT:
      c = bro_json_value_new_object ();
      for (guint i = 0; i < v->keys->len; i++)
        bro_json_value_set (c, v->keys->pdata[i], bro_scenario_fill_root (v->items->pdata[i]));
      return c;
    default:
      return bro_json_value_ref (v);
    }
}

GBytes *
bro_scenario_file_bytes (const BroJsonValue *content)
{
  gsize n;
  guint8 *bytes;
  int seed;
  const char *descriptor;
  if (content->kind == BRO_JSON_VALUE_STRING)
    return g_bytes_new (content->string, strlen (content->string));
  n = (gsize) bro_json_value_get_integer (J (content, "bytes"), 0);
  seed = (int) bro_json_value_get_integer (J (content, "seed"), 0);
  bytes = g_malloc0 (MAX (n, 1));
  for (gsize i = 0; i < n; i++)
    bytes[i] = (guint8) (((gint64) i * 31 + seed * 7) & 0xFF);
  descriptor = S (content, "descriptor", NULL);
  if (descriptor && n >= 32769 + strlen (descriptor))
    memcpy (bytes + 32769, descriptor, strlen (descriptor));
  return g_bytes_new_take (bytes, n);
}

/* A path the scenario gives relative to the root, as an absolute one ({root} stays for the fill). */
static char *
absolute (const char *path)
{
  return path[0] == '/' || g_str_has_prefix (path, "{root}") ? g_strdup (path) : g_strconcat ("{root}/", path, NULL);
}

BroJsonValue *
bro_scenario_world_configuration (BroScenarioWorld *w)
{
  g_autofree char *file = g_strconcat ("config/", S (w->given, "config", DEFAULT_CONFIG), NULL);
  g_autoptr (BroJsonValue) config = bro_test_fixture_json (file);
  BroJsonValue *patch, *libraries, *profile;
  if ((patch = J (w->given, "configPatch")))
    {
      BroJsonValue *merged = bro_scenario_merge_patch (config, patch);
      bro_json_value_unref (config);
      config = merged;
    }
  if ((libraries = J (w->given, "libraries")) && libraries->kind == BRO_JSON_VALUE_ARRAY)
    {
      g_autoptr (BroJsonValue) p = bro_json_value_new_object ();
      BroJsonValue *list = bro_json_value_new_array (), *merged;
      for (guint i = 0; i < libraries->items->len; i++)
        {
          BroJsonValue *l = libraries->items->pdata[i], *entry = bro_json_value_new_object ();
          g_autofree char *path = absolute (S (l, "path", ""));
          bro_json_value_set (entry, "id", bro_json_value_ref (J (l, "id")));
          bro_json_value_set (entry, "name", bro_json_value_ref (J (l, "name") ? J (l, "name") : J (l, "id")));
          bro_json_value_set (entry, "path", bro_json_value_new_string (path));
          bro_json_value_append (list, entry);
        }
      bro_json_value_set (p, "libraries", list);
      merged = bro_scenario_merge_patch (config, p);
      bro_json_value_unref (config);
      config = merged;
    }
  if ((profile = J (w->given, "profile")))
    {
      BroJsonValue *had = J (config, "profiles"), *profiles = bro_json_value_new_array (), *merged;
      g_autoptr (BroJsonValue) p = bro_json_value_new_object ();
      gboolean found = FALSE;
      for (guint i = 0; i < bro_json_value_length (had); i++)
        {
          BroJsonValue *one = bro_json_value_at (had, i);
          if (!found && g_strcmp0 (S (one, "id", NULL), "default") == 0)
            {
              found = TRUE;
              bro_json_value_append (profiles, bro_scenario_merge_patch (one, profile));
            }
          else
            bro_json_value_append (profiles, bro_json_value_ref (one));
        }
      if (!found)
        {
          g_autoptr (BroJsonValue) base = bro_json_value_new_object ();
          bro_json_value_set (base, "id", bro_json_value_new_string ("default"));
          bro_json_value_append (profiles, bro_scenario_merge_patch (base, profile));
        }
      bro_json_value_set (p, "profiles", profiles);
      merged = bro_scenario_merge_patch (config, p);
      bro_json_value_unref (config);
      config = merged;
    }
  return bro_scenario_fill_root (config);
}

static BroDiscContent
content_of (const BroJsonValue *disc)
{
  BroDiscContent c = BRO_DISC_CONTENT_VIDEO;
  if (!disc)
    return BRO_DISC_CONTENT_UNKNOWN;
  if (!bro_disc_content_from_wire (S (disc, "content", "video"), &c))
    c = BRO_DISC_CONTENT_VIDEO;
  return c;
}

static void
send (BroScenarioWorld *w, BroDeviceEventKind kind, const char *device)
{
  g_autoptr (BroDeviceEvent) e = bro_device_event_new ();
  e->kind = kind;
  e->device = g_strdup (device);
  test_fake_drives_send (w->root->drives, e);
}

static void
on_eject (const char *device, gpointer data)
{
  BroScenarioWorld *w = data;
  g_mutex_lock (&w->lock);
  for (guint i = 0; i < w->drives->len; i++)
    if (strcmp (((BroScenarioDrive *) w->drives->pdata[i])->device, device) == 0)
      ((BroScenarioDrive *) w->drives->pdata[i])->tray_open = TRUE; /* the disc stays in the open tray until a remove */
  g_mutex_unlock (&w->lock);
  send (w, BRO_DEVICE_EVENT_TRAY_OPENED, device); /* and the OS reports it, as for openTray */
}

static void
on_close_tray (const char *device, gpointer data)
{
  BroScenarioWorld *w = data;
  g_mutex_lock (&w->lock);
  for (guint i = 0; i < w->drives->len; i++)
    {
      BroScenarioDrive *d = w->drives->pdata[i];
      if (strcmp (d->device, device) == 0)
        d->tray_open = !d->tray_motor;
    }
  g_mutex_unlock (&w->lock);
}

static void
put (TestCompositionRoot *root, const char *path, GBytes *bytes)
{
  test_memory_fs_put (root->fs, path, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
}

static void
put_text (TestCompositionRoot *root, const char *path, const char *text)
{
  test_memory_fs_put (root->fs, path, text, strlen (text));
}

static BroJsonValue *
number_of (const BroJsonValue *unit, const char *key)
{
  BroJsonValue *v = J (unit, key);
  return v && v->kind == BRO_JSON_VALUE_INTEGER ? bro_json_value_new_integer (v->integer) : bro_json_value_new_null ();
}

static gboolean
int_of (const BroJsonValue *unit, const char *key, int *out)
{
  BroJsonValue *v = J (unit, key);
  if (!v || v->kind != BRO_JSON_VALUE_INTEGER)
    return FALSE;
  *out = (int) v->integer;
  return TRUE;
}

static char *
trim_slash (const char *path)
{
  char *p = g_strdup (path);
  gsize n = strlen (p);
  while (n > 0 && p[n - 1] == '/')
    p[--n] = '\0';
  return p;
}

/* An archive unit from before (given.units, README "Catalogue"): its folder with its files ("contents of <name>, long
 * enough to hash"), SHA256SUMS and a version 3 record bromelia-<unit8>.json, and unless inCatalogue is false the rows a
 * commit makes (bro_unit_repository_finish_commit: the committed unit and its files). Its id is
 * 00000000-0000-4000-8000-2000000000<n>; its library is given.library, else the one whose folder holds it; its
 * fingerprint is given, or fingerprintOf's (the Domain fingerprint of that disc's generated listing). */
static gboolean
add_unit (BroScenarioWorld *w, TestCompositionRoot *root, BroJsonValue *config, BroJsonValue *unit, int n, char **failure)
{
  BroJsonValue *libraries = J (config, "libraries"), *library = NULL, *of;
  g_autofree char *given_folder = trim_slash (S (unit, "folder", ""));
  g_autofree char *folder = g_strconcat (TEST_ROOT "/", given_folder, NULL);
  const char *wanted = S (unit, "library", NULL);
  g_autofree char *library_path = NULL;
  g_autofree char *fingerprint = NULL;
  g_autofree char *unit8 = NULL;
  g_autofree char *record_file = NULL;
  g_autofree char *sums_text = NULL;
  g_autofree char *sums_path = NULL;
  g_autofree char *record_path = NULL;
  g_autofree char *archived = NULL;
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_unit_file_free);
  g_autoptr (GPtrArray) sums = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_sum_entry_free);
  g_autoptr (BroJsonValue) document = NULL;
  g_autoptr (BroJsonValue) disc = NULL;
  g_autoptr (BroJsonValue) listed = NULL;
  g_autoptr (BroArchiveRecord) record = NULL;
  g_autoptr (GBytes) encoded = NULL;
  g_autoptr (BroUnitRecord) row = NULL;
  g_autoptr (BroCommitIntent) intent = NULL;
  g_autoptr (BroLibraryRecord) library_row = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *text = NULL;
  const char *status = S (unit, "status", "success"), *kind = S (unit, "kind", "movie"), *library_id;
  const char *name = S (unit, "name", strrchr (folder, '/') + 1);
  BroInstant at = bro_clock_now (BRO_CLOCK (root->clock));
  BroId id = { { 0 } };
  BroJsonValue *items, *entry;
  gint64 total = 0;
  if (bro_json_value_get_integer (J (unit, "record"), 3) == 2)
    {
      *failure = bro_scenario_failure ("given.units[].record 2 (phase 5)", "not built");
      return FALSE;
    }
  for (guint i = 0; i < bro_json_value_length (libraries) && !library; i++)
    {
      BroJsonValue *l = bro_json_value_at (libraries, i);
      g_autofree char *path = trim_slash (S (l, "path", ""));
      g_autofree char *prefix = g_strconcat (path, "/", NULL);
      if (wanted ? g_strcmp0 (S (l, "id", NULL), wanted) == 0 : g_str_has_prefix (folder, prefix))
        library = l;
    }
  if (!library)
    {
      g_autofree char *what = g_strconcat ("a library holding ", folder, NULL);
      *failure = bro_scenario_failure (what, "none");
      return FALSE;
    }
  library_id = S (library, "id", "");
  library_path = trim_slash (S (library, "path", ""));
  g_snprintf (id.value, sizeof id.value, "00000000-0000-4000-8000-2000000000%02x", n);
  unit8 = g_strndup (id.value, 8);
  if (S (unit, "fingerprint", NULL))
    fingerprint = g_strdup (S (unit, "fingerprint", NULL));
  else if ((of = J (unit, "fingerprintOf")))
    {
      g_autoptr (BroListing) listing = NULL;
      disc = bro_scenario_catalog_disc (of);
      listing = bro_scenario_parsed_listing (disc);
      fingerprint = bro_fingerprint_of (listing);
    }
  if (S (unit, "archivedAt", NULL))
    {
      g_autofree char *day = g_strconcat (S (unit, "archivedAt", ""), "T20:00:00.000Z", NULL);
      bro_instant_parse (day, &at);
    }
  archived = bro_instant_format (at);

  test_memory_fs_put_folder (root->fs, folder);
  items = J (unit, "files");
  for (guint i = 0; i < bro_json_value_length (items); i++)
    {
      const char *f = bro_json_value_get_string (bro_json_value_at (items, i), "");
      g_autofree char *contents = g_strdup_printf ("contents of %s, long enough to hash", f);
      g_autofree char *path = g_strconcat (folder, "/", f, NULL);
      g_autofree char *lower = g_ascii_strdown (f, -1);
      BroUnitFile *file = bro_unit_file_new ();
      put_text (root, path, contents);
      file->path = g_strdup (f);
      file->size = (gint64) strlen (contents);
      file->sha256 = bro_sha256_sums_hash ((const guint8 *) contents, strlen (contents));
      file->role = g_strdup (g_str_has_suffix (lower, ".mkv") ? "title" : "");
      total += file->size;
      g_ptr_array_add (sums, bro_sum_entry_new (file->path, file->sha256));
      g_ptr_array_add (files, file);
    }
  sums_text = bro_sha256_sums_render (sums);
  sums_path = g_strconcat (folder, "/" BRO_SHA256_SUMS_FILE_NAME, NULL);
  put_text (root, sums_path, sums_text);
  record_file = g_strconcat ("bromelia-", unit8, ".json", NULL);

  document = bro_json_value_new_object ();
  bro_json_value_set (document, "format", bro_json_value_new_string ("bromelia-archive"));
  bro_json_value_set (document, "version", bro_json_value_new_integer (3));
  entry = bro_json_value_new_object ();
  bro_json_value_set (entry, "id", bro_json_value_new_string (id.value));
  bro_json_value_set (entry, "short", bro_json_value_new_string (unit8));
  bro_json_value_set (entry, "library", bro_json_value_new_string (library_id));
  bro_json_value_set (document, "unit", entry);
  bro_json_value_set (document, "status", bro_json_value_new_string (status));
  bro_json_value_set (document, "name", bro_json_value_new_string (name));
  bro_json_value_set (document, "kind", bro_json_value_new_string (kind));
  entry = bro_json_value_new_object ();
  bro_json_value_set (entry, "label", bro_json_value_new_string (S (unit, "label", "")));
  bro_json_value_set (entry, "fingerprint", fingerprint ? bro_json_value_new_string (fingerprint) : bro_json_value_new_null ());
  bro_json_value_set (entry, "season", number_of (unit, "season"));
  bro_json_value_set (entry, "part", number_of (unit, "part"));
  bro_json_value_set (entry, "volume", number_of (unit, "volume"));
  bro_json_value_set (entry, "disc", number_of (unit, "disc"));
  bro_json_value_set (document, "disc", entry);
  bro_json_value_set (document, "episodes", J (unit, "episodes") ? bro_json_value_ref (J (unit, "episodes")) : bro_json_value_new_array ());
  entry = bro_json_value_new_array ();
  for (guint i = 0; i < files->len; i++)
    {
      BroUnitFile *f = files->pdata[i];
      BroJsonValue *o = bro_json_value_new_object ();
      bro_json_value_set (o, "path", bro_json_value_new_string (f->path));
      bro_json_value_set (o, "size", bro_json_value_new_integer (f->size));
      bro_json_value_set (o, "sha256", bro_json_value_new_string (f->sha256));
      bro_json_value_append (entry, o);
    }
  bro_json_value_set (document, "files", entry);
  bro_json_value_set (document, "archivedAt", bro_json_value_new_string (archived));
  text = bro_json_value_to_text (document);
  record = bro_archive_record_codec_decode (text, -1);
  if (!record)
    {
      g_autofree char *what = g_strconcat ("a version 3 record for ", folder, NULL);
      *failure = bro_scenario_failure (what, "none");
      return FALSE;
    }
  encoded = bro_archive_record_codec_encode_v3 (record);
  record_path = g_strconcat (folder, "/", record_file, NULL);
  put (root, record_path, encoded);
  if (!bro_json_value_get_bool (J (unit, "inCatalogue"), TRUE))
    return TRUE;

  row = bro_unit_record_new ();
  row->id = id;
  row->library_id = g_strdup (library_id);
  row->path = g_strdup (folder + strlen (library_path) + 1);
  row->record_file = g_strdup (record_file);
  row->record_version = 3;
  row->state = BRO_UNIT_STATE_COMMITTED;
  bro_unit_status_from_wire (status, &row->status);
  row->name = g_strdup (name);
  bro_media_kind_from_wire (kind, &row->kind);
  row->format = g_strdup ("bluray");
  row->format_code = g_strdup ("BR");
  row->fingerprint = g_strdup (fingerprint);
  row->label = g_strdup (S (unit, "label", ""));
  row->has_season = int_of (unit, "season", &row->season);
  row->has_part = int_of (unit, "part", &row->part);
  row->has_volume = int_of (unit, "volume", &row->volume);
  row->has_disc = int_of (unit, "disc", &row->disc);
  row->makemkv_version = g_strdup ("");
  row->bytes = total;
  row->file_count = (int) files->len;
  row->attempts = 1;
  row->created_at = at;
  row->has_committed_at = TRUE;
  row->committed_at = at;
  intent = bro_commit_intent_new ();
  intent->unit_id = id;
  intent->job_id = id;
  intent->staging = g_strdup ("");
  intent->destination = g_strdup (folder);
  intent->created_at = at;
  /* The library's row as attaching it left it (the unit's row needs it), with the marker the folder holds. */
  library_row = bro_library_record_new ();
  library_row->id = g_strdup (library_id);
  library_row->name = g_strdup (S (library, "name", library_id));
  library_row->path = g_strdup (library_path);
  library_row->marker_id = g_strdup (g_hash_table_lookup (w->markers, library_id) ? g_hash_table_lookup (w->markers, library_id) : "none");
  library_row->attached_at = at;
  library_row->online = TRUE;
  library_row->has_last_seen_at = TRUE;
  library_row->last_seen_at = at;
  if (!bro_library_repository_upsert (bro_store_libraries (BRO_STORE (root->store)), library_row, &error)
      || !bro_unit_repository_finish_commit (bro_store_units (BRO_STORE (root->store)), intent, row, files, &error))
    {
      *failure = bro_scenario_failure ("the unit's catalogue rows", error ? error->code : "?");
      return FALSE;
    }
  return TRUE;
}

gboolean
bro_scenario_world_prepare (BroScenarioWorld *w, TestCompositionRoot *root, char **failure)
{
  g_autoptr (BroJsonValue) config = bro_scenario_world_configuration (w);
  g_autoptr (GBytes) config_bytes = bro_json_value_encode_canonical (config);
  BroJsonValue *libraries = J (config, "libraries"), *given_libraries = J (w->given, "libraries"), *files, *units, *free_bytes;
  w->root = root;
  put (root, TEST_ROOT_CONFIG_PATH, config_bytes);
  for (guint i = 0; i < bro_json_value_length (libraries); i++)
    {
      BroJsonValue *library = bro_json_value_at (libraries, i);
      const char *id = S (library, "id", ""), *path = S (library, "path", "");
      gboolean marker = TRUE;
      for (guint k = 0; k < bro_json_value_length (given_libraries); k++)
        if (g_strcmp0 (S (bro_json_value_at (given_libraries, k), "id", NULL), id) == 0)
          marker = bro_json_value_get_bool (J (bro_json_value_at (given_libraries, k), "marker"), TRUE);
      test_memory_fs_put_folder (root->fs, path);
      if (marker)
        {
          g_autoptr (BroLibraryMarker) m = bro_library_marker_make (id, path, S (library, "name", id), bro_clock_now (BRO_CLOCK (root->clock)));
          g_autoptr (GBytes) bytes = bro_library_marker_encode (m);
          g_autofree char *file = g_strconcat (path, "/" BRO_LIBRARY_MARKER_FILE_NAME, NULL);
          put (root, file, bytes);
          g_hash_table_replace (w->markers, g_strdup (id), g_strdup (m->id));
        }
    }
  files = J (w->given, "files");
  for (guint i = 0; files && files->kind == BRO_JSON_VALUE_OBJECT && i < files->keys->len; i++)
    {
      g_autofree char *path = g_strconcat (TEST_ROOT "/", (char *) files->keys->pdata[i], NULL);
      g_autoptr (GBytes) bytes = bro_scenario_file_bytes (files->items->pdata[i]);
      put (root, path, bytes);
    }
  if ((free_bytes = J (w->given, "freeBytes")) && free_bytes->kind == BRO_JSON_VALUE_INTEGER)
    root->fs->any_volume->free_bytes = free_bytes->integer;
  units = J (w->given, "units");
  for (guint i = 0; i < bro_json_value_length (units); i++)
    if (!add_unit (w, root, config, bro_json_value_at (units, i), (int) i + 1, failure))
      return FALSE;
  if (w->disc)
    {
      g_autofree char *text = g_strconcat ("an image of ", S (w->disc, "volume", "a disc"), NULL);
      put_text (root, BRO_SCENARIO_ISO, text);
    }
  for (guint i = 0; i < w->drives->len; i++)
    {
      BroScenarioDrive *d = w->drives->pdata[i];
      g_autoptr (BroOsDrive) os = bro_os_drive_new (d->device, d->identification, NULL);
      test_fake_drives_add (root->drives, os);
      test_fake_drives_set_content (root->drives, d->device, content_of (d->disc));
    }
  test_fake_drives_on_tray (root->drives, on_eject, on_close_tray, w);
  return TRUE;
}

/* ---- the drive world ---- */

gboolean
bro_scenario_world_insert (BroScenarioWorld *w, const char *name, BroJsonValue *disc, char **failure)
{
  BroScenarioDrive *d = bro_scenario_world_drive_named (w, name, failure);
  BroJsonValue *read;
  BroDiscContent content;
  if (!d)
    return FALSE;
  read = bro_scenario_catalog_disc (disc);
  g_mutex_lock (&w->lock);
  g_clear_pointer (&d->disc, bro_json_value_unref);
  d->disc = read;
  d->tray_open = FALSE;
  content = content_of (d->disc);
  g_mutex_unlock (&w->lock);
  test_fake_drives_set_content (w->root->drives, d->device, content);
  send (w, BRO_DEVICE_EVENT_MEDIA_ARRIVED, d->device);
  return TRUE;
}

gboolean
bro_scenario_world_remove (BroScenarioWorld *w, const char *name, char **failure)
{
  BroScenarioDrive *d = bro_scenario_world_drive_named (w, name, failure);
  if (!d)
    return FALSE;
  g_mutex_lock (&w->lock);
  g_clear_pointer (&d->disc, bro_json_value_unref);
  g_mutex_unlock (&w->lock);
  test_fake_drives_set_content (w->root->drives, d->device, BRO_DISC_CONTENT_UNKNOWN);
  send (w, BRO_DEVICE_EVENT_MEDIA_REMOVED, d->device);
  return TRUE;
}

gboolean
bro_scenario_world_open_tray (BroScenarioWorld *w, const char *name, char **failure)
{
  BroScenarioDrive *d = bro_scenario_world_drive_named (w, name, failure);
  if (!d)
    return FALSE;
  g_mutex_lock (&w->lock);
  d->tray_open = TRUE;
  g_mutex_unlock (&w->lock);
  send (w, BRO_DEVICE_EVENT_TRAY_OPENED, d->device);
  return TRUE;
}

static int
by_index (gconstpointer a, gconstpointer b)
{
  return (*(BroScenarioDrive *const *) a)->index - (*(BroScenarioDrive *const *) b)->index;
}

GStrv
bro_scenario_world_drive_scan (BroScenarioWorld *w)
{
  g_autoptr (GPtrArray) order = g_ptr_array_new ();
  GPtrArray *lines = g_ptr_array_new ();
  g_mutex_lock (&w->lock);
  for (guint i = 0; i < w->drives->len; i++)
    g_ptr_array_add (order, w->drives->pdata[i]);
  g_ptr_array_sort (order, by_index);
  for (guint i = 0; i < order->len; i++)
    {
      BroScenarioDrive *d = order->pdata[i];
      const BroJsonValue *disc = d->tray_open ? NULL : d->disc;
      int state = disc ? 2 : d->tray_open ? 1 : 0, flags = 0;
      const char *type = disc ? S (disc, "type", NULL) : NULL;
      BroJsonValue *given = disc ? J (disc, "flags") : NULL;
      if (type)
        flags = !strcmp (type, "dvd") ? 1 : !strcmp (type, "hddvd") ? 2 : 12;
      if (given && given->kind == BRO_JSON_VALUE_INTEGER)
        flags = (int) given->integer;
      g_ptr_array_add (lines, g_strdup_printf ("DRV:%d,%d,999,%d,\"%s\",\"%s\",\"%s\"", d->index, state, flags, d->identification,
                                               disc ? S (disc, "label", S (disc, "volume", "")) : "", d->device));
    }
  g_mutex_unlock (&w->lock);
  g_ptr_array_add (lines, NULL);
  return (GStrv) g_ptr_array_free (lines, FALSE);
}

BroJsonValue *
bro_scenario_world_disc_of (BroScenarioWorld *w, const char *source)
{
  BroJsonValue *found = NULL;
  g_mutex_lock (&w->lock);
  if (g_str_has_prefix (source, "disc:") || g_str_has_prefix (source, "dev:"))
    {
      gboolean by_index = g_str_has_prefix (source, "disc:");
      char *end = NULL;
      gint64 index = by_index ? g_ascii_strtoll (source + 5, &end, 10) : -1;
      for (guint i = 0; i < w->drives->len; i++)
        {
          BroScenarioDrive *d = w->drives->pdata[i];
          if (by_index ? (end && !*end && d->index == index) : strcmp (d->device, source + 4) == 0)
            {
              found = !d->tray_open && d->disc ? bro_json_value_ref (d->disc) : NULL;
              break;
            }
        }
    }
  else
    found = w->disc ? bro_json_value_ref (w->disc) : NULL;
  g_mutex_unlock (&w->lock);
  return found;
}

char *
bro_scenario_world_device_of (BroScenarioWorld *w, const char *source)
{
  char *device = NULL;
  if (g_str_has_prefix (source, "dev:"))
    return g_strdup (source + 4);
  if (!g_str_has_prefix (source, "disc:"))
    return g_strdup ("");
  g_mutex_lock (&w->lock);
  for (guint i = 0; i < w->drives->len && !device; i++)
    {
      BroScenarioDrive *d = w->drives->pdata[i];
      if (g_ascii_strtoll (source + 5, NULL, 10) == d->index)
        device = g_strdup (d->device);
    }
  g_mutex_unlock (&w->lock);
  return device ? device : g_strdup ("");
}

/* ---- listings ---- */

int
bro_scenario_seconds (const char *duration)
{
  g_auto (GStrv) parts = g_strsplit (duration, ":", -1);
  int total = 0;
  for (guint i = 0; parts[i]; i++)
    total = total * 60 + (int) g_ascii_strtoll (parts[i], NULL, 10);
  return total;
}

char *
bro_scenario_listing (const BroJsonValue *disc, int min_length)
{
  const char *fixture = S (disc, "listing", NULL);
  BroJsonValue *titles = J (disc, "titles"), *kept = bro_json_value_new_array ();
  g_autoptr (BroJsonValue) d = bro_json_value_new_object ();
  if (fixture)
    {
      bro_json_value_unref (kept);
      return bro_test_fixture_text (fixture, NULL);
    }
  for (guint i = 0; i < bro_json_value_length (titles); i++)
    {
      BroJsonValue *t = bro_json_value_at (titles, i);
      const char *duration = t->kind == BRO_JSON_VALUE_ARRAY ? bro_json_value_get_string (bro_json_value_at (t, 0), "0") : S (t, "duration", "0");
      if (min_length < 0 || bro_scenario_seconds (duration) >= min_length)
        bro_json_value_append (kept, bro_json_value_ref (t));
    }
  bro_json_value_set (d, "volume", J (disc, "volume") ? bro_json_value_ref (J (disc, "volume")) : bro_json_value_new_string (""));
  bro_json_value_set (d, "type", J (disc, "type") ? bro_json_value_ref (J (disc, "type")) : bro_json_value_new_string ("bluray"));
  bro_json_value_set (d, "titles", kept);
  return bro_test_generated_listing (d);
}

BroListing *
bro_scenario_parsed_listing (const BroJsonValue *disc)
{
  g_autofree char *text = bro_scenario_listing (disc, -1);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  g_autoptr (BroListingBuilder) builder = bro_listing_builder_new ();
  for (guint i = 0; lines[i]; i++)
    {
      g_autoptr (BroRobotEvent) e = NULL;
      if (!*lines[i])
        continue;
      if ((e = bro_robot_parse_line (lines[i])))
        bro_listing_builder_feed (builder, e);
    }
  return bro_listing_builder_build (builder);
}
