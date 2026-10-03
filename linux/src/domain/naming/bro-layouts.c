/* bro-layouts.c */
#include "bro-layouts.h"
#include "bro-sanitizer.h"
#include "bro-template-engine.h"

#include <string.h>

#define MEDIA_SERVER_FOLDER "{libraryFolder}/{name}{releaseYear? ({releaseYear})}"
#define MEDIA_SERVER_MAIN "{name}{releaseYear? ({releaseYear})}"
#define MEDIA_SERVER_EPISODE \
  "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}{episodeTitle? - {episodeTitle}}"
#define MEDIA_SERVER_OTHER "Other/{name}{releaseYear? ({releaseYear})} - {track}"
#define MEDIA_SERVER_BACKUP "Backup/{name}{releaseYear? ({releaseYear})} - Backup - {format}"

static GHashTable *
copy_values (GHashTable *values)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, values);
  while (g_hash_table_iter_next (&it, &k, &v))
    g_hash_table_insert (t, g_strdup (k), g_strdup (v));
  return t;
}

char *
bro_layouts_folder (const BroNamingSettings *naming, GHashTable *values)
{
  if (naming->layout != BRO_LAYOUT_MEDIA_SERVER)
    return bro_template_engine_render_path (naming->folder_template, values);
  g_autoptr (GHashTable) v = copy_values (values);
  const char *lf = g_hash_table_lookup (v, "libraryFolder");
  if (!lf || !lf[0])
    g_hash_table_insert (v, g_strdup ("libraryFolder"),
                         g_strdup (g_strcmp0 (g_hash_table_lookup (v, "kind"), "tv") == 0 ? "TV Shows" : "Movies"));
  return bro_template_engine_render_path (MEDIA_SERVER_FOLDER, v);
}

static const char *
media_server_template (const BroPlannedOutput *o, GHashTable *v)
{
  if (o->role == BRO_PATH_ROLE_BACKUP || o->role == BRO_PATH_ROLE_IMAGE)
    return MEDIA_SERVER_BACKUP;
  const char *e = g_hash_table_lookup (v, "episodeNumber");
  if (o->role == BRO_PATH_ROLE_EPISODE || (e && e[0]))
    return MEDIA_SERVER_EPISODE;
  if (o->main_feature && g_strcmp0 (g_hash_table_lookup (v, "kind"), "movie") == 0)
    return MEDIA_SERVER_MAIN;
  return MEDIA_SERVER_OTHER;
}

static char *
templates_file (const BroNamingSettings *naming, const BroPlannedOutput *o, GHashTable *v)
{
  if (o->role == BRO_PATH_ROLE_BACKUP && naming->backup_subfolder[0])
    return bro_template_engine_render_path (naming->backup_subfolder, v);
  if (!naming->file_name_template[0]) {
    const char *original = g_hash_table_lookup (v, "original");
    return bro_sanitizer_component (original ? original : "");
  }
  return bro_template_engine_render_path (naming->file_name_template, v);
}

GPtrArray *
bro_layouts_paths (const BroNamingSettings *naming, GHashTable *values, GPtrArray *outputs)
{
  g_autofree char *unit = bro_layouts_folder (naming, values);
  GPtrArray *result = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_planned_path_free);
  for (guint i = 0; i < outputs->len; i++) {
    const BroPlannedOutput *o = outputs->pdata[i];
    g_autoptr (GHashTable) v = copy_values (values);
    GHashTableIter it;
    gpointer k, val;
    g_hash_table_iter_init (&it, o->values);
    while (g_hash_table_iter_next (&it, &k, &val))
      g_hash_table_insert (v, g_strdup (k), g_strdup (val));
    g_autofree char *file = naming->layout == BRO_LAYOUT_MEDIA_SERVER
                                ? bro_template_engine_render_path (media_server_template (o, v), v)
                                : templates_file (naming, o, v);
    BroPlannedPath *p = g_new0 (BroPlannedPath, 1);
    p->path = g_strconcat (unit, unit[0] && file[0] ? "/" : "", file, o->extension, NULL);
    p->role = o->role;
    p->title = o->title;
    p->episode = o->episode;
    g_ptr_array_add (result, p);
  }
  return result;
}
