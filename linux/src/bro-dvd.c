/* bro-dvd.c — DVD navigation analysis for splitting "play all" titles into episodes. */
#include "bro-dvd.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR BRO_DVD_SECTOR
#define NTSC 1.001 /* IFO times count 30 fps; NTSC playback is 29.97 fps */

/* ---- readers ---- */

struct _BroVideoTS {
  char *label;
  GHashTable *sizes; /* upper-case name -> gint64* */
  GHashTable *lba;   /* ISO: name -> gint64* */
  GHashTable *paths; /* folder: name -> char* */
  FILE *iso;
};

typedef struct {
  const guint8 *d;
  gsize n;
} Buf;

static int u8 (Buf b, gsize o) { return o < b.n ? b.d[o] : 0; }
static int u16 (Buf b, gsize o) { return (u8 (b, o) << 8) | u8 (b, o + 1); }
static int u32 (Buf b, gsize o) { return (int) (((guint) u16 (b, o) << 16) | (guint) u16 (b, o + 2)); }
static gint64 u32le (Buf b, gsize o) { return (gint64) ((guint32) u8 (b, o) | (guint32) u8 (b, o + 1) << 8 | (guint32) u8 (b, o + 2) << 16 | (guint32) u8 (b, o + 3) << 24); }
static Buf slice (Buf b, gsize from) { return from < b.n ? (Buf) { b.d + from, b.n - from } : (Buf) { NULL, 0 }; }

static GBytes *
iso_read (FILE *f, gint64 lba, int count)
{
  guint8 *buf = g_malloc ((gsize) count * SECTOR);
  size_t got = 0;
  if (fseeko (f, (off_t) lba * SECTOR, SEEK_SET) == 0)
    got = fread (buf, 1, (size_t) count * SECTOR, f);
  return g_bytes_new_take (buf, got);
}

static GBytes *
read_sectors (BroVideoTS *r, const char *file, gint64 sector, int count)
{
  if (r->iso)
    {
      gint64 *lba = g_hash_table_lookup (r->lba, file);
      return lba ? iso_read (r->iso, *lba + sector, count) : g_bytes_new (NULL, 0);
    }
  const char *path = g_hash_table_lookup (r->paths, file);
  FILE *f = path ? g_fopen (path, "rb") : NULL;
  GBytes *b;
  if (!f)
    return g_bytes_new (NULL, 0);
  b = iso_read (f, sector, count);
  fclose (f);
  return b;
}

static GBytes *
read_all (BroVideoTS *r, const char *file)
{
  gint64 *size = g_hash_table_lookup (r->sizes, file);
  GBytes *b, *cut;
  if (!size)
    return g_bytes_new (NULL, 0);
  b = read_sectors (r, file, 0, (int) ((*size + SECTOR - 1) / SECTOR));
  if ((gint64) g_bytes_get_size (b) <= *size)
    return b;
  cut = g_bytes_new_from_bytes (b, 0, *size);
  g_bytes_unref (b);
  return cut;
}

static gint64 *
boxed (gint64 v)
{
  gint64 *p = g_new (gint64, 1);
  *p = v;
  return p;
}

static BroVideoTS *
reader_new (void)
{
  BroVideoTS *r = g_new0 (BroVideoTS, 1);
  r->sizes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r->lba = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r->paths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return r;
}

void
bro_videots_free (BroVideoTS *r)
{
  if (!r)
    return;
  if (r->iso)
    fclose (r->iso);
  g_free (r->label);
  g_hash_table_unref (r->sizes);
  g_hash_table_unref (r->lba);
  g_hash_table_unref (r->paths);
  g_free (r);
}

const char *
bro_videots_label (BroVideoTS *r)
{
  return r->label;
}

typedef struct {
  char *name;
  gint64 lba, size;
} DirEntry;

static void
dir_entry_free (DirEntry *e)
{
  g_free (e->name);
  g_free (e);
}

static GPtrArray *
iso_directory (FILE *f, gint64 lba, gint64 size)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) dir_entry_free);
  g_autoptr (GBytes) bytes = iso_read (f, lba, (int) ((size + SECTOR - 1) / SECTOR));
  Buf d = { g_bytes_get_data (bytes, NULL), MIN ((gsize) size, g_bytes_get_size (bytes)) };
  gsize i = 0;
  while (i < d.n)
    {
      int len = d.d[i];
      if (len == 0)
        {
          i = (i / SECTOR + 1) * SECTOR;
          continue;
        }
      if (i + len > d.n || len < 34)
        break;
      int nlen = d.d[i + 32];
      if (33 + nlen <= len)
        {
          char *name = g_strndup ((const char *) d.d + i + 33, nlen);
          char *semi = strchr (name, ';');
          if (semi) *semi = '\0';
          if (!(nlen == 1 && (name[0] == 0 || name[0] == 1)))
            {
              DirEntry *e = g_new0 (DirEntry, 1);
              e->name = name;
              e->lba = u32le (d, i + 2);
              e->size = u32le (d, i + 10);
              g_ptr_array_add (out, e);
            }
          else
            g_free (name);
        }
      i += len;
    }
  return out;
}

BroVideoTS *
bro_videots_open_iso (const char *path)
{
  FILE *f = g_fopen (path, "rb");
  g_autoptr (GBytes) pvd_b = NULL;
  g_autoptr (GPtrArray) root = NULL;
  BroVideoTS *r;
  Buf pvd;
  DirEntry *vts = NULL;
  if (!f)
    return NULL;
  pvd_b = iso_read (f, 16, 1);
  pvd = (Buf) { g_bytes_get_data (pvd_b, NULL), g_bytes_get_size (pvd_b) };
  if (pvd.n < 190 || memcmp (pvd.d + 1, "CD001", 5) != 0)
    {
      fclose (f);
      return NULL;
    }
  root = iso_directory (f, u32le (pvd, 156 + 2), u32le (pvd, 156 + 10));
  for (guint i = 0; i < root->len; i++)
    if (g_ascii_strcasecmp (((DirEntry *) root->pdata[i])->name, "VIDEO_TS") == 0)
      vts = root->pdata[i];
  if (!vts)
    {
      fclose (f);
      return NULL;
    }
  r = reader_new ();
  r->iso = f;
  r->label = g_strstrip (g_strndup ((const char *) pvd.d + 40, 32));
  {
    g_autoptr (GPtrArray) files = iso_directory (f, vts->lba, vts->size);
    for (guint i = 0; i < files->len; i++)
      {
        DirEntry *e = files->pdata[i];
        g_hash_table_insert (r->sizes, g_ascii_strup (e->name, -1), boxed (e->size));
        g_hash_table_insert (r->lba, g_ascii_strup (e->name, -1), boxed (e->lba));
      }
  }
  return r;
}

BroVideoTS *
bro_videots_open_folder (const char *path, const char *label)
{
  g_autofree char *dir = g_strdup (path);
  g_autofree char *base = g_path_get_basename (path);
  g_autoptr (GDir) d = NULL;
  const char *name;
  BroVideoTS *r;
  if (!g_file_test (path, G_FILE_TEST_IS_DIR))
    return NULL;
  if (g_ascii_strcasecmp (base, "VIDEO_TS") != 0)
    {
      g_autoptr (GDir) parent = g_dir_open (path, 0, NULL);
      gboolean found = FALSE;
      while (parent && (name = g_dir_read_name (parent)))
        if (g_ascii_strcasecmp (name, "VIDEO_TS") == 0)
          {
            g_free (dir);
            dir = g_build_filename (path, name, NULL);
            found = TRUE;
            break;
          }
      if (!found)
        return NULL;
    }
  d = g_dir_open (dir, 0, NULL);
  if (!d)
    return NULL;
  r = reader_new ();
  while ((name = g_dir_read_name (d)))
    {
      g_autofree char *full = g_build_filename (dir, name, NULL);
      GStatBuf st;
      if (g_stat (full, &st) != 0)
        continue;
      g_hash_table_insert (r->sizes, g_ascii_strup (name, -1), boxed (st.st_size));
      g_hash_table_insert (r->paths, g_ascii_strup (name, -1), g_strdup (full));
    }
  if (!g_hash_table_contains (r->sizes, "VIDEO_TS.IFO"))
    {
      bro_videots_free (r);
      return NULL;
    }
  if (label && *label)
    r->label = g_strdup (label);
  else
    {
      g_autofree char *parent = g_path_get_dirname (dir);
      r->label = g_path_get_basename (parent);
    }
  return r;
}

/* ---- VM commands ---- */

gboolean
bro_dvd_decode_jump (const guint8 *c, BroDvdJump *out)
{
  static const char *const ops[] = { "?", "&", "==", "!=", ">=", ">", "<=", "<" };
  Buf b = { c, 8 };
  memset (out, 0, sizeof *out);
  out->title_number = -1;
  if (c[0] >> 5 != 1)
    return FALSE;
  if (c[1] & 0x70)
    {
      g_autofree char *val = (c[1] & 0x80) ? g_strdup_printf ("%d", u16 (b, 4)) : g_strdup_printf ("GPRM%d", c[5] & 0xF);
      out->condition = g_strdup_printf ("if GPRM%d %s %s: ", c[3] & 0xF, ops[(c[1] >> 4) & 7], val);
    }
  else
    out->condition = g_strdup ("");
  if (!(c[0] & 0x10))
    {
      if ((c[1] & 0x0F) == 5) /* LinkPTTN */
        {
          out->chapter = TRUE;
          out->target = u16 (b, 6) & 0x3FF;
          return TRUE;
        }
    }
  else
    {
      int sub = c[1] & 0x0F;
      if (sub == 5) /* JumpVTS_PTT */
        {
          out->chapter = TRUE;
          out->title_number = c[5] & 0x7F;
          out->target = u16 (b, 2) & 0x3FF;
          return TRUE;
        }
      if (sub == 2) /* JumpTT */
        {
          out->target = c[5] & 0x7F;
          return TRUE;
        }
    }
  g_clear_pointer (&out->condition, g_free);
  return FALSE;
}

static GPtrArray *
pgc_commands (Buf pgc)
{
  GPtrArray *out = g_ptr_array_new ();
  int off = u16 (pgc, 0xE4);
  if (!off)
    return out;
  int n = u16 (pgc, off) + u16 (pgc, off + 2) + u16 (pgc, off + 4);
  for (int k = 0; k < n; k++)
    if ((gsize) off + 16 + 8 * k <= pgc.n)
      g_ptr_array_add (out, (gpointer) (pgc.d + off + 8 + 8 * k));
  return out;
}

static GArray *
pgcs (Buf table)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (Buf));
  for (int i = 0; i < u16 (table, 0); i++)
    {
      Buf p = slice (table, u32 (table, 8 + 8 * i + 4));
      g_array_append_val (out, p);
    }
  return out;
}

static GArray *
menu_pgcs (Buf ifo, int pointer)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (Buf));
  int sec = u32 (ifo, pointer);
  if (!sec)
    return out;
  Buf ut = slice (ifo, (gsize) sec * SECTOR);
  for (int i = 0; i < u16 (ut, 0); i++)
    {
      g_autoptr (GArray) p = pgcs (slice (ut, u32 (ut, 8 + 8 * i + 4)));
      g_array_append_vals (out, p->data, p->len);
    }
  return out;
}

static gboolean
is_nav (Buf d, gsize o)
{
  return o + 0x2D <= d.n && u32 (d, o + 0x0E) == 0x1BB && u32 (d, o + 0x26) == 0x1BF && d.d[o + 0x2C] == 0;
}

/* Button commands of a menu VOB (8-byte commands, owned by the returned array). */
static GPtrArray *
button_commands (BroVideoTS *r, const char *vob)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gint64 *size = g_hash_table_lookup (r->sizes, vob);
  gint64 total = size ? *size / SECTOR : 0;
  for (gint64 s = 0; s < total; s += 512)
    {
      g_autoptr (GBytes) chunk = read_sectors (r, vob, s, (int) MIN (512, total - s));
      Buf d = { g_bytes_get_data (chunk, NULL), g_bytes_get_size (chunk) };
      if (!d.n)
        break;
      for (gsize k = 0; k < d.n / SECTOR; k++)
        {
          gsize o = k * SECTOR, pci = o + 0x2D;
          if (!is_nav (d, o))
            continue;
          int buttons = MIN (u8 (d, pci + 0x60 + 16), 36);
          for (int b = 0; b < buttons; b++)
            {
              gsize at = pci + 0x8E + 18 * b + 10;
              if (at + 8 > d.n)
                continue;
              char *hex = g_strdup_printf ("%02x%02x%02x%02x%02x%02x%02x%02x", d.d[at], d.d[at + 1], d.d[at + 2], d.d[at + 3],
                                           d.d[at + 4], d.d[at + 5], d.d[at + 6], d.d[at + 7]);
              if (g_hash_table_add (seen, hex))
                g_ptr_array_add (out, g_memdup2 (d.d + at, 8));
            }
        }
    }
  return out;
}

/* ---- analysis ---- */

static void
dvd_title_free (BroDvdTitle *t)
{
  g_array_unref (t->chapters);
  g_array_unref (t->vobs);
  g_free (t);
}

void
bro_dvd_analysis_free (BroDvdAnalysis *a)
{
  if (!a)
    return;
  g_free (a->label);
  g_hash_table_unref (a->titles);
  g_hash_table_unref (a->jumps);
  g_free (a);
}

static void
add_jump (BroDvdAnalysis *a, int title, int ptt, char *how)
{
  GHashTable *m = g_hash_table_lookup (a->jumps, GINT_TO_POINTER (title));
  if (!m)
    {
      m = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
      g_hash_table_insert (a->jumps, GINT_TO_POINTER (title), m);
    }
  if (!g_hash_table_contains (m, GINT_TO_POINTER (ptt)))
    g_hash_table_insert (m, GINT_TO_POINTER (ptt), how);
  else
    g_free (how);
}

static double
dvd_time (Buf d, gsize o)
{
#define BCD(x) (((x) >> 4) * 10 + ((x) & 0xF))
  double fps = (u8 (d, o + 3) >> 6) == 1 ? 25.0 : 30.0;
  return BCD (u8 (d, o)) * 3600 + BCD (u8 (d, o + 1)) * 60 + BCD (u8 (d, o + 2)) + BCD (u8 (d, o + 3) & 0x3F) / fps;
#undef BCD
}

BroDvdAnalysis *
bro_dvd_analyse (BroVideoTS *r)
{
  g_autoptr (GBytes) vmg_b = read_all (r, "VIDEO_TS.IFO");
  Buf vmg = { g_bytes_get_data (vmg_b, NULL), g_bytes_get_size (vmg_b) };
  BroDvdAnalysis *a;
  Buf tt;
  int ntitles;
  g_autoptr (GHashTable) vts_of = g_hash_table_new (g_direct_hash, g_direct_equal);  /* title -> vts */
  g_autoptr (GHashTable) by_key = g_hash_table_new (g_direct_hash, g_direct_equal);  /* vts<<8|ttn -> title */

  if (vmg.n <= 0xCC || memcmp (vmg.d, "DVDVIDEO-VMG", 12) != 0)
    return NULL;
  a = g_new0 (BroDvdAnalysis, 1);
  a->label = g_strdup (r->label);
  a->titles = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) dvd_title_free);
  a->jumps = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_hash_table_unref);

  tt = slice (vmg, (gsize) u32 (vmg, 0xC4) * SECTOR);
  ntitles = u16 (tt, 0);
  for (int i = 0; i < ntitles; i++)
    {
      int vts = u8 (tt, 8 + 12 * i + 6), ttn = u8 (tt, 8 + 12 * i + 7);
      g_hash_table_insert (vts_of, GINT_TO_POINTER (i + 1), GINT_TO_POINTER (vts));
      g_hash_table_insert (by_key, GINT_TO_POINTER (vts << 8 | ttn), GINT_TO_POINTER (i + 1));
    }

  {
    g_autoptr (GArray) menus = menu_pgcs (vmg, 0xC8);
    for (guint i = 0; i < menus->len; i++)
      {
        g_autoptr (GPtrArray) cmds = pgc_commands (g_array_index (menus, Buf, i));
        for (guint k = 0; k < cmds->len; k++)
          {
            BroDvdJump j;
            if (bro_dvd_decode_jump (cmds->pdata[k], &j) && !j.chapter)
              add_jump (a, j.target, 1, g_strdup ("menu JumpTT"));
            g_free (j.condition);
          }
      }
    g_autoptr (GPtrArray) buttons = button_commands (r, "VIDEO_TS.VOB");
    for (guint k = 0; k < buttons->len; k++)
      {
        BroDvdJump j;
        if (bro_dvd_decode_jump (buttons->pdata[k], &j) && !j.chapter)
          add_jump (a, j.target, 1, g_strdup ("button JumpTT"));
        g_free (j.condition);
      }
  }

  for (int vtsn = 1; vtsn <= 99; vtsn++)
    {
      gboolean used = FALSE;
      for (int t = 1; t <= ntitles && !used; t++)
        used = GPOINTER_TO_INT (g_hash_table_lookup (vts_of, GINT_TO_POINTER (t))) == vtsn;
      if (!used)
        continue;
      g_autofree char *name = g_strdup_printf ("VTS_%02d_0.IFO", vtsn);
      g_autofree char *vob = g_strdup_printf ("VTS_%02d_0.VOB", vtsn);
      if (!g_hash_table_contains (r->sizes, name))
        continue;
      g_autoptr (GBytes) ifo_b = read_all (r, name);
      Buf ifo = { g_bytes_get_data (ifo_b, NULL), g_bytes_get_size (ifo_b) };
      Buf pb = slice (ifo, (gsize) u32 (ifo, 0xC8) * SECTOR);
      int nttu = u16 (pb, 0);
      g_autoptr (GArray) offs = g_array_new (FALSE, FALSE, sizeof (int));
      g_autoptr (GArray) title_pgcs = pgcs (slice (ifo, (gsize) u32 (ifo, 0xCC) * SECTOR));
      for (int i = 0; i < nttu; i++)
        {
          int o = u32 (pb, 8 + 4 * i);
          g_array_append_val (offs, o);
        }
      {
        int end = u32 (pb, 4) + 1;
        g_array_append_val (offs, end);
      }
      for (int ttn = 1; ttn <= nttu; ttn++)
        {
          int title = GPOINTER_TO_INT (g_hash_table_lookup (by_key, GINT_TO_POINTER (vtsn << 8 | ttn)));
          int first_pgc = -1;
          BroDvdTitle *dt;
          if (!title)
            continue;
          dt = g_new0 (BroDvdTitle, 1);
          dt->chapters = g_array_new (FALSE, FALSE, sizeof (double));
          dt->vobs = g_array_new (FALSE, FALSE, sizeof (int));
          for (int j = g_array_index (offs, int, ttn - 1); j + 4 <= g_array_index (offs, int, ttn); j += 4)
            {
              int pgcn = u16 (pb, j), pgn = u16 (pb, j + 2);
              if (pgcn < 1 || pgcn > (int) title_pgcs->len)
                continue;
              if (first_pgc < 0)
                first_pgc = pgcn;
              Buf pg = g_array_index (title_pgcs, Buf, pgcn - 1);
              int nprog = u8 (pg, 2), ncell = u8 (pg, 3);
              int pmo = u16 (pg, 0xE6), cpbo = u16 (pg, 0xE8), cpso = u16 (pg, 0xEA);
              g_autoptr (GArray) pmap = g_array_new (FALSE, FALSE, sizeof (int));
              for (int k = 0; k < nprog; k++)
                {
                  int v2 = u8 (pg, pmo + k);
                  g_array_append_val (pmap, v2);
                }
              {
                int e = ncell + 1;
                g_array_append_val (pmap, e);
              }
              if (pgn < 1 || pgn >= (int) pmap->len)
                continue;
              double d = 0;
              for (int cell = MAX (0, g_array_index (pmap, int, pgn - 1) - 1); cell < MAX (0, g_array_index (pmap, int, pgn) - 1); cell++)
                d += dvd_time (pg, cpbo + 24 * cell + 4);
              d *= NTSC;
              g_array_append_val (dt->chapters, d);
              int vobid = u16 (pg, cpso + 4 * (g_array_index (pmap, int, pgn - 1) - 1));
              g_array_append_val (dt->vobs, vobid);
            }
          g_hash_table_insert (a->titles, GINT_TO_POINTER (title), dt);
          if (first_pgc > 0)
            {
              g_autoptr (GPtrArray) cmds = pgc_commands (g_array_index (title_pgcs, Buf, first_pgc - 1));
              for (guint k = 0; k < cmds->len; k++)
                {
                  BroDvdJump j;
                  if (bro_dvd_decode_jump (cmds->pdata[k], &j) && j.chapter && j.title_number < 0)
                    add_jump (a, title, j.target, g_strdup_printf ("%sLinkPTTN %d", j.condition, j.target));
                  g_free (j.condition);
                }
            }
        }
      {
        g_autoptr (GArray) menus = menu_pgcs (ifo, 0xD0);
        g_autoptr (GPtrArray) buttons = button_commands (r, vob);
        g_autoptr (GPtrArray) all = g_ptr_array_new ();
        for (guint i = 0; i < menus->len; i++)
          {
            g_autoptr (GPtrArray) cmds = pgc_commands (g_array_index (menus, Buf, i));
            for (guint k = 0; k < cmds->len; k++)
              g_ptr_array_add (all, cmds->pdata[k]);
          }
        for (guint k = 0; k < buttons->len; k++)
          g_ptr_array_add (all, buttons->pdata[k]);
        for (guint k = 0; k < all->len; k++)
          {
            BroDvdJump j;
            if (bro_dvd_decode_jump (all->pdata[k], &j) && j.chapter && j.title_number >= 0)
              {
                int t = GPOINTER_TO_INT (g_hash_table_lookup (by_key, GINT_TO_POINTER (vtsn << 8 | j.title_number)));
                if (t)
                  add_jump (a, t, j.target, g_strdup_printf ("menu JumpVTS_PTT %d:%d", j.title_number, j.target));
              }
            g_free (j.condition);
          }
      }
    }
  return a;
}

/* ---- plans ---- */

void
bro_episode_plan_free (BroEpisodePlan *p)
{
  if (!p)
    return;
  g_array_unref (p->starts);
  g_free (p->end_rule);
  g_array_unref (p->tail);
  g_array_unref (p->chapter_starts);
  g_array_unref (p->episode_durations);
  g_ptr_array_unref (p->reasons);
  g_free (p);
}

void
bro_episode_plan_range (BroEpisodePlan *p, guint i, int *first, int *last)
{
  int s = g_array_index (p->starts, int, i);
  int end = i + 1 < p->starts->len ? g_array_index (p->starts, int, i + 1) - 1 : p->last_end;
  *first = s;
  *last = MAX (s, end);
}

GArray *
bro_episode_plan_split_chapters (BroEpisodePlan *p)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 1; i < p->starts->len; i++)
    g_array_append_val (out, g_array_index (p->starts, int, i));
  if (p->tail->len)
    g_array_append_val (out, g_array_index (p->tail, int, 0));
  return out;
}

gboolean
bro_episode_plan_plausible (BroEpisodePlan *p, gboolean strict)
{
  double lo = G_MAXDOUBLE, hi = 0;
  if ((int) p->starts->len < (strict ? 3 : 2) || !p->episode_durations->len)
    return FALSE;
  for (guint i = 0; i < p->episode_durations->len; i++)
    {
      double d = g_array_index (p->episode_durations, double, i);
      lo = MIN (lo, d);
      hi = MAX (hi, d);
    }
  if (lo <= 0)
    return FALSE;
  return strict ? (lo >= 900 && hi / lo <= 1.35) : (lo >= 300 && hi / lo <= 2.0);
}

gboolean
bro_episode_plan_matches_chapters (BroEpisodePlan *p, int chapters)
{
  return chapters >= p->kept_chapters - 1 && chapters <= (int) p->chapter_starts->len;
}

static int
cmp_int (gconstpointer a, gconstpointer b)
{
  return *(const int *) a - *(const int *) b;
}

BroEpisodePlan *
bro_dvd_plan (BroDvdAnalysis *a, int title)
{
  BroDvdTitle *t = g_hash_table_lookup (a->titles, GINT_TO_POINTER (title));
  GHashTable *jumps = g_hash_table_lookup (a->jumps, GINT_TO_POINTER (title));
  BroEpisodePlan *p;
  GHashTableIter it;
  gpointer k, v;
  int nch, last, last_end;
  double acc = 0;

  if (!t || !t->chapters->len || !jumps)
    return NULL;
  nch = t->chapters->len;
  p = g_new0 (BroEpisodePlan, 1);
  p->title = title;
  p->starts = g_array_new (FALSE, FALSE, sizeof (int));
  p->tail = g_array_new (FALSE, FALSE, sizeof (int));
  p->chapter_starts = g_array_new (FALSE, FALSE, sizeof (double));
  p->episode_durations = g_array_new (FALSE, FALSE, sizeof (double));
  p->reasons = g_ptr_array_new_with_free_func (g_free);
  g_hash_table_iter_init (&it, jumps);
  while (g_hash_table_iter_next (&it, &k, &v))
    {
      int c = GPOINTER_TO_INT (k);
      if (c >= 1 && c <= nch)
        g_array_append_val (p->starts, c);
    }
  if (!g_hash_table_contains (jumps, GINT_TO_POINTER (1)))
    {
      int one = 1;
      g_array_append_val (p->starts, one);
    }
  g_array_sort (p->starts, cmp_int);
  if (p->starts->len < 2)
    {
      bro_episode_plan_free (p);
      return NULL;
    }
#define CH(n) g_array_index (t->chapters, double, (n) - 1)
#define VOB(i) g_array_index (t->vobs, int, (i))
  last = g_array_index (p->starts, int, p->starts->len - 1);
  last_end = last;
  {
    gboolean several = FALSE;
    for (guint i = 1; i < t->vobs->len; i++)
      several |= VOB (i) != VOB (0);
    if (several)
      {
        while (last_end < nch && VOB (last_end) == VOB (last - 1))
          last_end++;
        p->end_rule = g_strdup ("VOB boundary");
      }
    else
      {
        last_end = MIN (nch, last + (last - g_array_index (p->starts, int, p->starts->len - 2)) - 1);
        p->end_rule = g_strdup ("same chapter count as the previous episode");
      }
  }
  p->last_end = last_end;
  for (int n = last_end + 1; n <= nch; n++)
    if (CH (n) >= 1.0)
      g_array_append_val (p->tail, n);
  for (int n = 1; n <= nch; n++)
    {
      g_array_append_val (p->chapter_starts, acc);
      acc += CH (n);
      if (!(n > last_end && CH (n) < 1.0))
        {
          p->duration += CH (n);
          p->kept_chapters++;
        }
    }
  for (guint i = 0; i < p->starts->len; i++)
    {
      int f, l;
      double d = 0;
      const char *how = g_hash_table_lookup (jumps, GINT_TO_POINTER (g_array_index (p->starts, int, i)));
      g_ptr_array_add (p->reasons, g_strdup (how ? how : "title start"));
      bro_episode_plan_range (p, i, &f, &l);
      for (int n = f; n <= l; n++)
        d += CH (n);
      g_array_append_val (p->episode_durations, d);
    }
#undef CH
#undef VOB
  return p;
}

static int
cmp_plans (gconstpointer a, gconstpointer b)
{
  const BroEpisodePlan *x = *(BroEpisodePlan *const *) a, *y = *(BroEpisodePlan *const *) b;
  if (x->starts->len != y->starts->len)
    return (int) y->starts->len - (int) x->starts->len;
  return x->title - y->title;
}

GPtrArray *
bro_dvd_plans (BroDvdAnalysis *a)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_episode_plan_free);
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init (&it, a->jumps);
  while (g_hash_table_iter_next (&it, &k, NULL))
    {
      BroEpisodePlan *p = bro_dvd_plan (a, GPOINTER_TO_INT (k));
      if (p)
        g_ptr_array_add (out, p);
    }
  g_ptr_array_sort (out, cmp_plans);
  return out;
}

/* ---- episode numbers ---- */

typedef struct {
  int first, last;
} Cell;

static int
cmp_cells (gconstpointer a, gconstpointer b)
{
  return ((const Cell *) a)->first - ((const Cell *) b)->first;
}

GPtrArray *
bro_dvd_menu_stills (BroVideoTS *r)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  g_autoptr (GPtrArray) names = g_hash_table_get_keys_as_ptr_array (r->sizes);
  g_autoptr (GRegex) re = g_regex_new ("^VTS_\\d\\d_0\\.VOB$", 0, 0, NULL);
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
  for (guint i = 0; i < names->len; i++)
    {
      const char *name = names->pdata[i];
      gint64 size = *(gint64 *) g_hash_table_lookup (r->sizes, name);
      if (!(g_str_equal (name, "VIDEO_TS.VOB") || g_regex_match (re, name, 0, NULL)) || size < 64 * 1024 || size > 512LL * 1024 * 1024)
        continue;
      g_autoptr (GBytes) all = read_all (r, name);
      Buf d = { g_bytes_get_data (all, NULL), g_bytes_get_size (all) };
      gsize total = d.n / SECTOR;
      g_autoptr (GHashTable) cells = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
      g_autoptr (GArray) bounds = g_array_new (FALSE, FALSE, sizeof (Cell));
      GHashTableIter it;
      gpointer v;
      for (gsize s = 0; s < total; s++)
        {
          if (!is_nav (d, s * SECTOR))
            continue;
          gsize dsi = s * SECTOR + 0x407;
          int key = u16 (d, dsi + 0x18) << 8 | u8 (d, dsi + 0x1B);
          Cell *c = g_hash_table_lookup (cells, GINT_TO_POINTER (key));
          if (!c)
            {
              c = g_new (Cell, 1);
              c->first = (int) s;
              g_hash_table_insert (cells, GINT_TO_POINTER (key), c);
            }
          c->last = (int) s;
        }
      g_hash_table_iter_init (&it, cells);
      while (g_hash_table_iter_next (&it, NULL, &v))
        g_array_append_val (bounds, *(Cell *) v);
      g_array_sort (bounds, cmp_cells);
      for (guint b = 0; b < bounds->len; b++)
        {
          int start = g_array_index (bounds, Cell, b).first;
          int end = b + 1 < bounds->len ? g_array_index (bounds, Cell, b + 1).first : (int) total;
          if (end - start > 8)
            g_ptr_array_add (out, g_bytes_new_from_bytes (all, (gsize) start * SECTOR, (gsize) (end - start) * SECTOR));
        }
    }
  return out;
}

GArray *
bro_dvd_episode_numbers (const char *text)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GRegex) re = g_regex_new ("EPIS[O0]DE\\s*#?\\s*(\\d{1,4})\\b", G_REGEX_CASELESS, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  g_regex_match (re, text ? text : "", 0, &m);
  while (g_match_info_matches (m))
    {
      g_autofree char *s = g_match_info_fetch (m, 1);
      int n = atoi (s);
      gboolean dup = FALSE;
      for (guint i = 0; i < out->len; i++)
        dup |= g_array_index (out, int, i) == n;
      if (!dup)
        g_array_append_val (out, n);
      g_match_info_next (m, NULL);
    }
  return out;
}

int
bro_dvd_first_episode (GArray *numbers, int count)
{
  int best = -1, winner = -1, winners = 0;
  if (!numbers || !numbers->len || count <= 0)
    return -1;
  g_autoptr (GHashTable) seen = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (guint i = 0; i < numbers->len; i++)
    for (int k = 0; k < count; k++)
      {
        int s = g_array_index (numbers, int, i) - k;
        int score = 0;
        if (s < 0 || !g_hash_table_add (seen, GINT_TO_POINTER (s + 1)))
          continue;
        for (guint j = 0; j < numbers->len; j++)
          {
            int n = g_array_index (numbers, int, j);
            score += s <= n && n < s + count;
          }
        if (score > best) { best = score; winner = s; winners = 1; }
        else if (score == best) winners++;
      }
  return winners == 1 && best >= 2 ? winner : -1;
}

GArray *
bro_dvd_mkv_chapters (GArray *disc, BroEpisodePlan *p, GArray *mkv)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
  if (!mkv || !mkv->len)
    {
      g_array_append_vals (out, disc->data, disc->len);
      return out;
    }
  for (guint i = 0; i < disc->len; i++)
    {
      double want = g_array_index (p->chapter_starts, double, g_array_index (disc, int, i) - 1);
      guint best = 0;
      for (guint k = 1; k < mkv->len; k++)
        if (ABS (g_array_index (mkv, double, k) - want) < ABS (g_array_index (mkv, double, best) - want))
          best = k;
      if (ABS (g_array_index (mkv, double, best) - want) > 1.0)
        {
          g_array_unref (out);
          return NULL;
        }
      int n = (int) best + 1;
      g_array_append_val (out, n);
    }
  return out;
}

char *
bro_dvd_hms (double s)
{
  int whole = (int) s;
  return g_strdup_printf ("%d:%02d:%06.3f", whole / 3600, whole % 3600 / 60, s - (whole / 60) * 60);
}

GArray *
bro_parse_simple_chapters (const char *text)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GRegex) re = g_regex_new ("^CHAPTER\\d+=(\\d+):(\\d+):([\\d.]+)", G_REGEX_MULTILINE, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  g_regex_match (re, text ? text : "", 0, &m);
  while (g_match_info_matches (m))
    {
      g_autofree char *h = g_match_info_fetch (m, 1), *mi = g_match_info_fetch (m, 2), *sec = g_match_info_fetch (m, 3);
      double v = atoi (h) * 3600 + atoi (mi) * 60 + g_ascii_strtod (sec, NULL);
      g_array_append_val (out, v);
      g_match_info_next (m, NULL);
    }
  return out;
}
