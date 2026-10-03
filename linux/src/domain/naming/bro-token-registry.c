/* bro-token-registry.c */
#include "bro-token-registry.h"
#include "bro-label-parser.h"

#include <string.h>

static void
set (GHashTable *t, const char *key, char *value)
{
  g_hash_table_insert (t, g_strdup (key), value);
}

static char *
n (int v)
{
  return v >= 0 ? g_strdup_printf ("%d", v) : g_strdup ("");
}

GHashTable *
bro_token_registry_values (const BroIdentity *id, const BroTokenContext *c)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  const BroLocalTime *lt = c->has_local_time ? &c->local_time : NULL;
  set (t, "name", g_strdup (id->name));
  set (t, "kind", g_strdup (bro_media_kind_to_wire (id->kind)));
  set (t, "format", g_strdup (id->format_code.text));
  set (t, "rip", g_strdup (c->rip ? c->rip : ""));
  set (t, "discLabel", bro_label_parser_set_description (id->label));
  set (t, "discNumber", n (id->label->disc));
  set (t, "season", n (id->label->season));
  set (t, "part", n (id->label->part));
  set (t, "volumeNumber", n (id->label->volume));
  set (t, "disc", g_strdup (c->disc && c->disc[0] ? c->disc : c->volume ? c->volume : ""));
  set (t, "volume", g_strdup (c->volume ? c->volume : ""));
  set (t, "type", g_strdup (bro_disc_type_to_wire (c->type)));
  set (t, "drive", g_strdup (c->drive ? c->drive : ""));
  set (t, "job", g_strdup (c->job ? c->job : ""));
  set (t, "date", lt ? g_strdup_printf ("%04d-%02d-%02d", lt->year, lt->month, lt->day) : g_strdup (""));
  set (t, "time", lt ? g_strdup_printf ("%02d-%02d-%02d", lt->hour, lt->minute, lt->second) : g_strdup (""));
  set (t, "year", lt ? g_strdup_printf ("%04d", lt->year) : g_strdup (""));
  set (t, "month", lt ? g_strdup_printf ("%02d", lt->month) : g_strdup (""));
  set (t, "day", lt ? g_strdup_printf ("%02d", lt->day) : g_strdup (""));
  set (t, "releaseYear", n (c->release_year));
  set (t, "seasonOr1", n (id->label->season >= 0 ? id->label->season : 1));
  set (t, "libraryFolder", g_strdup (id->kind == BRO_MEDIA_KIND_TV ? "TV Shows" : "Movies"));
  set (t, "episode", g_strdup (""));
  set (t, "episodeNumber", g_strdup (""));
  set (t, "episodeTitle", g_strdup (""));
  set (t, "track", g_strdup (""));
  return t;
}

char *
bro_token_registry_track_label (const BroTitle *title)
{
  const char *src = title->source_file;
  gsize len = strlen (src);
  if (len >= 5 && g_ascii_strcasecmp (src + len - 5, ".mpls") == 0) {
    const char *slash = strrchr (src, '/'), *back = strrchr (src, '\\');
    const char *file = MAX (slash, back) ? MAX (slash, back) + 1 : src;
    return g_strdup_printf ("Playlist %.*s", (int) (strlen (file) - 5), file);
  }
  return g_strdup_printf ("Title %d", title->has_source_title_id ? title->source_title_id : title->index);
}

char *
bro_token_registry_episode_label (int episode, int width)
{
  return g_strdup_printf ("Episode %0*d", width, episode);
}
