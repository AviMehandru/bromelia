/* bro-identity.c — disc format, movie / show identity, plugin matching and checksums. */
#include "bro-identity.h"
#include "bro-logic.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

const char *const bro_format_codes[] = { "DVD", "DVDe", "BR", "BRe", "4K", "4Ke", NULL };

char *
bro_format_code (BroDiscFormat f, gboolean encrypted)
{
  const char *b = f == BRO_FORMAT_DVD ? "DVD" : f == BRO_FORMAT_BLURAY ? "BR" : f == BRO_FORMAT_UHD ? "4K"
                : f == BRO_FORMAT_HDDVD ? "HDDVD" : "DISC";
  return g_strconcat (b, encrypted ? "e" : "", NULL);
}

int
bro_format_key (BroDiscFormat f)
{
  switch (f)
    {
    case BRO_FORMAT_DVD: return BRO_FORMAT_KEY_DVD;
    case BRO_FORMAT_BLURAY: return BRO_FORMAT_KEY_BLURAY;
    case BRO_FORMAT_UHD: return BRO_FORMAT_KEY_UHD;
    default: return -1;
    }
}

const char *
bro_format_label (BroDiscFormat f)
{
  switch (f)
    {
    case BRO_FORMAT_DVD: return "DVD";
    case BRO_FORMAT_BLURAY: return "Blu-ray";
    case BRO_FORMAT_UHD: return "4K Ultra HD Blu-ray";
    case BRO_FORMAT_HDDVD: return "HD DVD";
    default: return "Disc";
    }
}

const char *
bro_format_token (BroDiscFormat f)
{
  switch (f)
    {
    case BRO_FORMAT_DVD: return "dvd";
    case BRO_FORMAT_BLURAY: return "bluray";
    case BRO_FORMAT_UHD: return "uhd";
    case BRO_FORMAT_HDDVD: return "hddvd";
    default: return "unknown";
    }
}

const char *bro_kind_label (BroMediaKind k) { return k == BRO_KIND_TV ? "TV show" : "Movie"; }
const char *bro_kind_token (BroMediaKind k) { return k == BRO_KIND_TV ? "tv" : "movie"; }

static gboolean
is_uhd (BroDiscInfo *info)
{
  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      for (guint j = 0; j < t->tracks->len; j++)
        {
          BroTrack *tr = t->tracks->pdata[j];
          const char *size = bro_track_attr (tr, BRO_ATTR_VIDEO_SIZE);
          g_autofree char *codec = NULL;
          if (bro_track_kind (tr) != BRO_TRACK_VIDEO)
            continue;
          codec = g_ascii_strup (g_strconcat (bro_track_attr (tr, BRO_ATTR_CODEC_ID) ? bro_track_attr (tr, BRO_ATTR_CODEC_ID) : "", " ",
                                              bro_track_attr (tr, BRO_ATTR_CODEC_SHORT) ? bro_track_attr (tr, BRO_ATTR_CODEC_SHORT) : "", NULL), -1);
          if ((size && (strstr (size, "2160") || strstr (size, "3840"))) || strstr (codec, "HEVC") || strstr (codec, "MPEGH"))
            return TRUE;
        }
    }
  return FALSE;
}

BroDiscFormat
bro_detect_format (BroDiscInfo *info, int flags)
{
  if (info)
    {
      g_autofree char *t = g_ascii_strdown (bro_disc_info_attr (info, BRO_ATTR_TYPE) ? bro_disc_info_attr (info, BRO_ATTR_TYPE) : "", -1);
      if (strstr (t, "blu"))
        return is_uhd (info) ? BRO_FORMAT_UHD : BRO_FORMAT_BLURAY;
      if (strstr (t, "hd"))
        return BRO_FORMAT_HDDVD;
      if (strstr (t, "dvd"))
        return BRO_FORMAT_DVD;
      if (is_uhd (info))
        return BRO_FORMAT_UHD;
    }
  if (flags >= 0)
    {
      if (flags & BRO_DISC_BLURAY) return BRO_FORMAT_BLURAY;
      if (flags & BRO_DISC_HDDVD) return BRO_FORMAT_HDDVD;
      if (flags & BRO_DISC_DVD) return BRO_FORMAT_DVD;
    }
  return BRO_FORMAT_UNKNOWN;
}

BroDiscFormat
bro_detect_backup_folder (const char *folder)
{
  g_autofree char *vts = g_build_filename (folder, "VIDEO_TS", NULL);
  g_autofree char *index = g_build_filename (folder, "BDMV", "index.bdmv", NULL);
  g_autofree char *head = NULL;
  gsize len = 0;
  if (g_file_test (vts, G_FILE_TEST_IS_DIR))
    return BRO_FORMAT_DVD;
  if (!g_file_get_contents (index, &head, &len, NULL) || len < 8 || strncmp (head, "INDX", 4) != 0)
    return BRO_FORMAT_UNKNOWN;
  return strncmp (head, "INDX0300", 8) == 0 ? BRO_FORMAT_UHD : BRO_FORMAT_BLURAY;
}

/* ---- labels ---- */

static const char *const noise[] = { "WS", "FS", "16X9", "4X3", "NTSC", "PAL", "R1", "R2", "R4", "UHD", "4K", "BD", "BLURAY", "BLU", "RAY",
                                     "DVD", "DVD5", "DVD9", "BD25", "BD50", "BD66", "BD100", "HDR", "SDR", "HD", "DISC", "DISK", NULL };
static const char *const small_words[] = { "a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into", "nor", "of", "on",
                                           "or", "the", "to", "vs", "with", NULL };

static gboolean
in_list (const char *const *list, const char *s)
{
  for (; *list; list++)
    if (g_str_equal (*list, s))
      return TRUE;
  return FALSE;
}

static int
number_token (const char *s)
{
  if (!s || !*s)
    return -1;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isdigit (*p))
      return -1;
  return strlen (s) > 6 ? -1 : atoi (s);
}

/* Matches `prefix` + 1..maxd digits exactly; returns the number or -1. */
static int
prefixed_number (const char *u, const char *prefix, int maxd)
{
  size_t pl = strlen (prefix);
  if (strncmp (u, prefix, pl) != 0)
    return -1;
  int n = number_token (u + pl);
  return n >= 0 && (int) strlen (u + pl) <= maxd ? n : -1;
}

static char *
title_case (GPtrArray *words)
{
  gboolean all_upper = TRUE, all_lower = TRUE;
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < words->len; i++)
    {
      const char *w = words->pdata[i];
      g_autofree char *up = g_utf8_strup (w, -1), *lo = g_utf8_strdown (w, -1);
      if (!g_str_equal (w, up)) all_upper = FALSE;
      if (!g_str_equal (w, lo)) all_lower = FALSE;
    }
  for (guint i = 0; i < words->len; i++)
    {
      const char *w = words->pdata[i];
      if (i) g_string_append_c (out, ' ');
      if (!(all_upper || all_lower))
        {
          g_string_append (out, w);
          continue;
        }
      g_autofree char *lo = g_utf8_strdown (w, -1);
      gboolean roman = strlen (lo) <= 4 && lo[strspn (lo, "ivx")] == '\0';
      if (i > 0 && in_list (small_words, lo))
        g_string_append (out, lo);
      else if (roman)
        {
          g_autofree char *up = g_utf8_strup (w, -1);
          g_string_append (out, up);
        }
      else
        {
          gboolean start = TRUE;
          for (const char *p = lo; *p; p = g_utf8_next_char (p))
            {
              gunichar c = g_utf8_get_char (p);
              g_string_append_unichar (out, start ? g_unichar_toupper (c) : c);
              start = c == '-';
            }
        }
    }
  return g_string_free (out, FALSE);
}

void
bro_label_parse (const char *raw, BroLabelInfo *info)
{
  GString *s = g_string_new (raw ? raw : "");
  g_autoptr (GPtrArray) tokens = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) title = g_ptr_array_new ();
  gboolean stopped = FALSE;
  char **parts;
  g_autoptr (GRegex) sd = g_regex_new ("^S(\\d{1,2})(?:D(\\d{1,2}))?(?:E(\\d{1,3}))?$", 0, 0, NULL);
  static const char *const episode_words[] = { "EP", "EPS", "EPISODE", "EPISODES", NULL };

  memset (info, 0, sizeof *info);
  info->season = info->part = info->volume = info->disc = -1;
  g_string_replace (s, "™", "", 0);
  g_string_replace (s, "®", "", 0);
  g_string_replace (s, "©", "", 0);
  g_string_replace (s, " - ", " ", 0);
  for (char *p = s->str; *p; p++)
    if (strchr ("_.\t()[],", *p))
      *p = ' ';
  parts = g_strsplit (s->str, " ", -1);
  for (char **p = parts; *p; p++)
    if (**p && !g_str_equal (*p, "-"))
      g_ptr_array_add (tokens, g_strdup (*p));
  g_strfreev (parts);
  g_string_free (s, TRUE);

  for (guint i = 0; i < tokens->len; i++)
    {
      const char *t = tokens->pdata[i];
      g_autofree char *u = g_ascii_strup (t, -1);
      int next = i + 1 < tokens->len ? number_token (tokens->pdata[i + 1]) : -1;
      gboolean marker = TRUE, consumed = FALSE;
      int n;
      g_autoptr (GMatchInfo) m = NULL;

      if (g_regex_match (sd, u, 0, &m))
        {
          g_autofree char *a = g_match_info_fetch (m, 1), *b = g_match_info_fetch (m, 2);
          info->season = atoi (a);
          if (b && *b) info->disc = atoi (b);
          info->looks_like_series = TRUE;
        }
      else if (g_str_equal (u, "SEASON") || prefixed_number (u, "SEASON", 2) >= 0)
        {
          if ((n = prefixed_number (u, "SEASON", 2)) >= 0) info->season = n;
          else if (next >= 0) { info->season = next; consumed = TRUE; }
          info->looks_like_series = TRUE;
        }
      else if (in_list (episode_words, u) || prefixed_number (u, "EP", 3) >= 0 || prefixed_number (u, "EPS", 3) >= 0
               || prefixed_number (u, "EPISODE", 3) >= 0 || prefixed_number (u, "EPISODES", 3) >= 0)
        {
          if (in_list (episode_words, u) && next >= 0) consumed = TRUE;
          info->looks_like_series = TRUE;
        }
      else if ((n = prefixed_number (u, "DISC", 2)) >= 0 || (n = prefixed_number (u, "DISK", 2)) >= 0 ||
               (n = prefixed_number (u, "CD", 2)) >= 0 || (n = prefixed_number (u, "D", 2)) >= 0)
        info->disc = n;
      else if ((g_str_equal (u, "DISC") || g_str_equal (u, "DISK") || g_str_equal (u, "D")) && next >= 0)
        { info->disc = next; consumed = TRUE; }
      else if ((n = prefixed_number (u, "PART", 2)) >= 0 || (n = prefixed_number (u, "PT", 2)) >= 0 || (n = prefixed_number (u, "P", 2)) >= 0)
        info->part = n;
      else if ((g_str_equal (u, "PART") || g_str_equal (u, "PT")) && next >= 0)
        { info->part = next; consumed = TRUE; }
      else if ((n = prefixed_number (u, "VOLUME", 2)) >= 0 || (n = prefixed_number (u, "VOL", 2)) >= 0 || (n = prefixed_number (u, "V", 2)) >= 0)
        { info->volume = n; info->looks_like_series = TRUE; }
      else if ((g_str_equal (u, "VOL") || g_str_equal (u, "VOLUME")) && next >= 0)
        { info->volume = next; consumed = TRUE; info->looks_like_series = TRUE; }
      else
        marker = FALSE;

      if (marker)
        stopped = TRUE;
      else if (!stopped && !in_list (noise, u))
        g_ptr_array_add (title, (gpointer) t);
      if (consumed)
        i++;
    }
  info->title = title_case (title);
}

void
bro_label_clear (BroLabelInfo *l)
{
  g_clear_pointer (&l->title, g_free);
}

char *
bro_label_set_description (const BroLabelInfo *l)
{
  GString *s = g_string_new (NULL);
#define PART(v, word) if ((v) >= 0) g_string_append_printf (s, "%s%s %d", s->len ? " " : "", word, v)
  PART (l->season, "Season");
  PART (l->part, "Part");
  PART (l->volume, "Volume");
  PART (l->disc, "Disc");
#undef PART
  return g_string_free (s, FALSE);
}

/* ---- identity ---- */

static int
cmp_int (gconstpointer a, gconstpointer b)
{
  return *(const int *) a - *(const int *) b;
}

GPtrArray *
bro_episode_like_titles (GPtrArray *titles)
{
  GPtrArray *out = g_ptr_array_new ();
  g_autoptr (GArray) d = g_array_new (FALSE, FALSE, sizeof (int));
  double median;
  for (guint i = 0; titles && i < titles->len; i++)
    {
      int s = bro_title_duration (titles->pdata[i]);
      if (s >= 600 && s <= 4500)
        g_array_append_val (d, s);
    }
  if (d->len < 2)
    return out;
  g_array_sort (d, cmp_int);
  median = g_array_index (d, int, d->len / 2);
  for (guint i = 0; i < titles->len; i++)
    {
      int s = bro_title_duration (titles->pdata[i]);
      if (s >= 600 && s <= 4500 && ABS (s - median) <= median * 0.35)
        g_ptr_array_add (out, titles->pdata[i]);
    }
  return out;
}

BroIdentity *
bro_identity_resolve (BroDiscInfo *info, const char *disc_label, int drive_flags, int format_override, gboolean encrypted,
                      const char *name_override, int kind_override, int play_all_episodes)
{
  BroIdentity *id = g_new0 (BroIdentity, 1);
  const char *vol = info && bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME) && *bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME)
                      ? bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME) : (disc_label ? disc_label : "");
  const char *disc_name = info ? bro_disc_info_name (info) : "";
  BroLabelInfo from_name;
  g_autofree char *upper = g_utf8_strup (disc_name, -1);
  gboolean human = *disc_name && !strchr (disc_name, '_') && !g_str_equal (disc_name, vol)
                   && (strchr (disc_name, ' ') || !g_str_equal (disc_name, upper));
  g_autofree char *over = g_strstrip (g_strdup (name_override ? name_override : ""));

  bro_label_parse (vol, &id->label);
  bro_label_parse (disc_name, &from_name);
  if (!*id->label.title)
    {
      g_free (id->label.title);
      id->label.title = g_strdup (from_name.title);
    }
  if (id->label.season < 0) id->label.season = from_name.season;
  if (id->label.disc < 0) id->label.disc = from_name.disc;
  if (id->label.part < 0) id->label.part = from_name.part;
  if (id->label.volume < 0) id->label.volume = from_name.volume;
  id->label.looks_like_series |= from_name.looks_like_series;

  if (*over)
    id->name = g_strdup (over);
  else if (human && *from_name.title)
    id->name = g_strdup (from_name.title);
  else if (*id->label.title)
    id->name = g_strdup (id->label.title);
  else
    id->name = g_strdup (disc_label && *disc_label ? disc_label : "Disc");
  bro_label_clear (&from_name);

  id->format = format_override >= 0 ? (BroDiscFormat) format_override : bro_detect_format (info, drive_flags);
  id->encrypted = encrypted;
  if (kind_override >= 0)
    {
      id->kind = kind_override;
      id->reason = g_strdup ("chosen by you");
    }
  else if (id->label.looks_like_series)
    {
      id->kind = BRO_KIND_TV;
      id->reason = g_strdup ("the disc label has season / volume markers");
    }
  else if (play_all_episodes >= 3)
    {
      id->kind = BRO_KIND_TV;
      id->reason = g_strdup_printf ("the disc menu plays %d episodes in one title", play_all_episodes);
    }
  else
    {
      g_autoptr (GPtrArray) eps = bro_episode_like_titles (info ? info->titles : NULL);
      id->kind = eps->len >= 3 ? BRO_KIND_TV : BRO_KIND_MOVIE;
      id->reason = g_strdup (eps->len >= 3 ? "the disc has several titles of episode length" : "no series markers or episode-length titles");
    }
  return id;
}

void
bro_identity_free (BroIdentity *id)
{
  if (!id)
    return;
  g_free (id->name);
  g_free (id->reason);
  bro_label_clear (&id->label);
  g_free (id);
}

gboolean
bro_identity_equal (const BroIdentity *a, const BroIdentity *b)
{
  if (!a || !b)
    return a == b;
  return g_str_equal (a->name, b->name) && a->kind == b->kind && a->format == b->format && a->encrypted == b->encrypted &&
         g_str_equal (a->label.title, b->label.title) && a->label.season == b->label.season && a->label.part == b->label.part &&
         a->label.volume == b->label.volume && a->label.disc == b->label.disc && g_str_equal (a->reason, b->reason);
}

char *
bro_identity_format_code (const BroIdentity *id)
{
  return bro_format_code (id->format, id->encrypted);
}

void
bro_identity_template_values (const BroIdentity *id, GHashTable *v, const char *rip)
{
  g_autofree char *code = bro_identity_format_code (id);
  g_autofree char *set = bro_label_set_description (&id->label);
#define NUM(key, n) do { g_autofree char *_s = (n) >= 0 ? g_strdup_printf ("%d", n) : g_strdup (""); bro_template_values_set (v, key, _s); } while (0)
  bro_template_values_set (v, "name", id->name);
  bro_template_values_set (v, "kind", bro_kind_token (id->kind));
  bro_template_values_set (v, "format", code);
  bro_template_values_set (v, "rip", rip);
  bro_template_values_set (v, "discLabel", set);
  NUM ("discNumber", id->label.disc);
  NUM ("season", id->label.season);
  NUM ("part", id->label.part);
  NUM ("volumeNumber", id->label.volume);
#undef NUM
  bro_template_values_set (v, "episode", "");
  bro_template_values_set (v, "episodeNumber", "");
  bro_template_values_set (v, "track", "");
}

char *
bro_track_label (BroTitle *t)
{
  const char *src = bro_title_str (t, BRO_ATTR_SOURCE_FILE_NAME);
  if (g_str_has_suffix (src, ".mpls") || g_str_has_suffix (src, ".MPLS"))
    return g_strdup_printf ("Playlist %.*s", (int) strlen (src) - 5, src);
  return g_strdup_printf ("Title %d", bro_title_source_id (t) >= 0 ? bro_title_source_id (t) : t->index);
}

char *
bro_episode_label (int n, int width)
{
  return g_strdup_printf ("Episode %0*d", width, n);
}

/* ---- plugins ---- */

gboolean
bro_plugin_matches (const BroPostStep *step, const char *name, const char *disc_label, const char *code)
{
  g_autofree char *pattern = g_strstrip (g_strdup (step->match_name ? step->match_name : ""));
  guint n = 0;
  if (*pattern)
    {
      g_autoptr (GRegex) re = g_regex_new (pattern, G_REGEX_CASELESS, 0, NULL);
      if (!re || !(g_regex_match (re, name ? name : "", 0, NULL) || g_regex_match (re, disc_label ? disc_label : "", 0, NULL)))
        return FALSE;
    }
  for (guint i = 0; step->match_formats && i < step->match_formats->len; i++)
    {
      g_autofree char *f = g_strstrip (g_strdup (step->match_formats->pdata[i]));
      size_t len = strlen (f);
      if (!len)
        continue;
      n++;
      if (f[len - 1] == '*' ? g_ascii_strncasecmp (code, f, len - 1) == 0 : g_ascii_strcasecmp (code, f) == 0)
        return TRUE;
    }
  return n == 0;
}

char *
bro_plugin_validate (const char *pattern)
{
  g_autofree char *p = g_strstrip (g_strdup (pattern ? pattern : ""));
  g_autoptr (GRegex) re = NULL;
  if (!*p)
    return NULL;
  re = g_regex_new (p, 0, 0, NULL);
  return re ? NULL : g_strdup ("Invalid regular expression");
}

/* ---- checksums ---- */

void
bro_checksum_free (BroChecksum *c)
{
  if (!c)
    return;
  g_free (c->path);
  g_free (c->sha256);
  g_free (c);
}

static void
walk (const char *full, const char *rel, GPtrArray *out)
{
  if (g_file_test (full, G_FILE_TEST_IS_DIR))
    {
      g_autoptr (GDir) d = g_dir_open (full, 0, NULL);
      const char *name;
      while (d && (name = g_dir_read_name (d)))
        {
          g_autofree char *f = g_build_filename (full, name, NULL);
          g_autofree char *r = g_strconcat (rel, "/", name, NULL);
          if (name[0] != '.')
            walk (f, r, out);
        }
    }
  else if (g_file_test (full, G_FILE_TEST_IS_REGULAR))
    {
      g_autoptr (GFile) gf = g_file_new_for_path (full);
      g_autoptr (GFileInfo) fi = g_file_query_info (gf, G_FILE_ATTRIBUTE_STANDARD_SIZE, 0, NULL, NULL);
      BroChecksum *c = g_new0 (BroChecksum, 1);
      c->path = g_strdup (rel);
      c->size = fi ? g_file_info_get_size (fi) : 0;
      g_ptr_array_add (out, c);
    }
}

static int
cmp_checksum (gconstpointer a, gconstpointer b)
{
  return strcmp ((*(BroChecksum *const *) a)->path, (*(BroChecksum *const *) b)->path);
}

GPtrArray *
bro_checksum_list_files (GPtrArray *items, const char *base)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_checksum_free);
  g_autofree char *b = g_canonicalize_filename (base, NULL);
  g_autofree char *prefix = g_strconcat (b, "/", NULL);
  for (guint i = 0; items && i < items->len; i++)
    {
      g_autofree char *full = g_canonicalize_filename (items->pdata[i], NULL);
      g_autofree char *rel = g_str_has_prefix (full, prefix) ? g_strdup (full + strlen (prefix)) : g_path_get_basename (full);
      walk (full, rel, out);
    }
  g_ptr_array_sort (out, cmp_checksum);
  for (guint i = 1; i < out->len;)
    if (g_str_equal (((BroChecksum *) out->pdata[i])->path, ((BroChecksum *) out->pdata[i - 1])->path))
      g_ptr_array_remove_index (out, i);
    else
      i++;
  return out;
}

char *
bro_sha256_file (const char *path, BroHashProgress progress, gpointer user_data, GError **error)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileInputStream) in = g_file_read (f, NULL, error);
  g_autoptr (GChecksum) sum = NULL;
  g_autofree guchar *buf = NULL;
  gint64 done = 0;
  gssize n;
  if (!in)
    return NULL;
  sum = g_checksum_new (G_CHECKSUM_SHA256);
  buf = g_malloc (8 << 20);
  while ((n = g_input_stream_read (G_INPUT_STREAM (in), buf, 8 << 20, NULL, error)) > 0)
    {
      g_checksum_update (sum, buf, n);
      done += n;
      if (progress && !progress (done, user_data))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          return NULL;
        }
    }
  if (n < 0)
    return NULL;
  return g_strdup (g_checksum_get_string (sum));
}

char *
bro_checksums_render (GPtrArray *entries)
{
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < entries->len; i++)
    {
      BroChecksum *c = entries->pdata[i];
      g_string_append_printf (s, "%s  %s\n", c->sha256, c->path);
    }
  return g_string_free (s, FALSE);
}

/* Parses "hash  path" / "hash *path" lines into path → hash. */
static GHashTable *
parse_sums (const char *text)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_auto (GStrv) lines = g_strsplit (text ? text : "", "\n", -1);
  for (char **l = lines; *l; l++)
    {
      size_t len = strlen (*l);
      if (len && (*l)[len - 1] == '\r') (*l)[--len] = '\0';
      if (len < 67 || (*l)[64] != ' ' || !((*l)[65] == ' ' || (*l)[65] == '*'))
        continue;
      gboolean hex = TRUE;
      for (int k = 0; k < 64; k++)
        hex &= g_ascii_isxdigit ((*l)[k]);
      if (hex)
        g_hash_table_insert (t, g_strdup (*l + 66), g_ascii_strdown (*l, 64));
    }
  return t;
}

char *
bro_checksums_write_merged (const char *base, GPtrArray *entries, GError **error)
{
  char *path = g_build_filename (base, BRO_CHECKSUM_FILE, NULL);
  g_autofree char *old = NULL;
  g_autoptr (GHashTable) t = NULL;
  g_autoptr (GPtrArray) keys = NULL;
  GString *s = g_string_new (NULL);
  g_file_get_contents (path, &old, NULL, NULL);
  t = parse_sums (old);
  for (guint i = 0; i < entries->len; i++)
    {
      BroChecksum *c = entries->pdata[i];
      g_hash_table_insert (t, g_strdup (c->path), g_strdup (c->sha256));
    }
  keys = g_hash_table_get_keys_as_ptr_array (t);
  g_ptr_array_sort_values (keys, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < keys->len; i++)
    g_string_append_printf (s, "%s  %s\n", (char *) g_hash_table_lookup (t, keys->pdata[i]), (char *) keys->pdata[i]);
  if (!bro_write_file_atomic (path, s->str, error))
    {
      g_string_free (s, TRUE);
      g_free (path);
      return NULL;
    }
  g_string_free (s, TRUE);
  return path;
}

GPtrArray *
bro_checksums_verify (const char *base, GError **error)
{
  g_autofree char *path = g_build_filename (base, BRO_CHECKSUM_FILE, NULL);
  g_autofree char *text = NULL;
  g_autoptr (GHashTable) t = NULL;
  g_autoptr (GPtrArray) keys = NULL;
  GPtrArray *bad;
  if (!g_file_get_contents (path, &text, NULL, error))
    return NULL;
  t = parse_sums (text);
  keys = g_hash_table_get_keys_as_ptr_array (t);
  g_ptr_array_sort_values (keys, (GCompareFunc) g_strcmp0);
  bad = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < keys->len; i++)
    {
      g_autofree char *full = g_build_filename (base, keys->pdata[i], NULL);
      g_autofree char *h = bro_sha256_file (full, NULL, NULL, NULL);
      if (!h || !g_str_equal (h, g_hash_table_lookup (t, keys->pdata[i])))
        g_ptr_array_add (bad, g_strdup (keys->pdata[i]));
    }
  return bad;
}
