/* bro-dvd-nav.c */
#include "bro-dvd-nav.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048
/* IFO times count 30 fps; NTSC playback is 29.97 fps. */
#define NTSC 1.001

/* ---- bounds-checked reads over a view (0 past the end) ---- */

typedef struct {
  const guint8 *d;
  gsize n;
} View;

static int
u8 (View v, gint64 o)
{
  return o >= 0 && (gsize) o < v.n ? v.d[o] : 0;
}

static int
u16 (View v, gint64 o)
{
  return (u8 (v, o) << 8) | u8 (v, o + 1);
}

static guint32
u32 (View v, gint64 o)
{
  return ((guint32) u16 (v, o) << 16) | (guint32) u16 (v, o + 2);
}

static View
slice (View v, gint64 from)
{
  if (from < 0 || (gsize) from >= v.n)
    return (View) { NULL, 0 };
  return (View) { v.d + from, v.n - from };
}

static View
slice_len (View v, gint64 from, gsize len)
{
  View s = slice (v, from);
  if (s.n > len)
    s.n = len;
  return s;
}

static View
view_of (GBytes *b)
{
  gsize n = 0;
  const guint8 *d = b ? g_bytes_get_data (b, &n) : NULL;
  return (View) { d, n };
}

static int
bcd (int b)
{
  return (b >> 4) * 10 + (b & 0xF);
}

static double
dvd_time (View d, gint64 o)
{
  double fps = (u8 (d, o + 3) >> 6) == 1 ? 25.0 : 30.0;
  return bcd (u8 (d, o)) * 3600 + bcd (u8 (d, o + 1)) * 60 + bcd (u8 (d, o + 2)) + bcd (u8 (d, o + 3) & 0x3F) / fps;
}

BroJump *
bro_dvd_nav_decode_jump (const guint8 *c, gsize length)
{
  if (length < 8 || c[0] >> 5 != 1)
    return NULL;
  View v = { c, length };
  g_autofree char *cond = NULL;
  if (c[1] & 0x70) {
    static const char *const ops[] = { "?", "&", "==", "!=", ">=", ">", "<=", "<" };
    g_autofree char *val = (c[1] & 0x80) ? g_strdup_printf ("%d", u16 (v, 4)) : g_strdup_printf ("GPRM%d", c[5] & 0xF);
    cond = g_strdup_printf ("if GPRM%d %s %s: ", c[3] & 0xF, ops[(c[1] >> 4) & 7], val);
  }
  BroJump *j = g_new0 (BroJump, 1);
  if (!(c[0] & 0x10)) {
    if ((c[1] & 0x0F) == 5) { /* LinkPTTN */
      j->kind = BRO_JUMP_PTT;
      j->chapter = u16 (v, 6) & 0x3FF;
      j->condition = g_strdup (cond ? cond : "");
      return j;
    }
  } else {
    int sub = c[1] & 0x0F;
    if (sub == 5) { /* JumpVTS_PTT */
      j->kind = BRO_JUMP_PTT;
      j->has_title_number = TRUE;
      j->title_number = c[5] & 0x7F;
      j->chapter = u16 (v, 2) & 0x3FF;
      j->condition = g_strdup (cond ? cond : "");
      return j;
    }
    if (sub == 2) { /* JumpTT */
      j->kind = BRO_JUMP_TITLE;
      j->number = c[5] & 0x7F;
      j->condition = g_strdup (cond ? cond : "");
      return j;
    }
  }
  g_free (j);
  return NULL;
}

/* Views of the commands of a PGC (into @pgc). */
static GArray *
pgc_commands (View pgc)
{
  GArray *list = g_array_new (FALSE, FALSE, sizeof (View));
  int off = u16 (pgc, 0xE4);
  if (off == 0)
    return list;
  int n = u16 (pgc, off) + u16 (pgc, off + 2) + u16 (pgc, off + 4);
  for (int k = 0; k < n; k++) {
    View c = slice_len (pgc, off + 8 + 8 * k, 8);
    g_array_append_val (list, c);
  }
  return list;
}

static void
add_pgcs (GArray *list, View table)
{
  for (int i = 0; i < u16 (table, 0); i++) {
    View p = slice (table, u32 (table, 8 + 8 * i + 4));
    g_array_append_val (list, p);
  }
}

static GArray *
menu_pgcs (View ifo, int pointer_offset)
{
  GArray *list = g_array_new (FALSE, FALSE, sizeof (View));
  guint32 sec = u32 (ifo, pointer_offset);
  if (sec == 0)
    return list;
  View ut = slice (ifo, (gint64) sec * SECTOR);
  for (int i = 0; i < u16 (ut, 0); i++)
    add_pgcs (list, slice (ut, u32 (ut, 8 + 8 * i + 4)));
  return list;
}

static gboolean
is_nav (View d, gint64 o)
{
  return (gsize) (o + 0x2D) <= d.n && u32 (d, o + 0x0E) == 0x1BB && u32 (d, o + 0x26) == 0x1BF && d.d[o + 0x2C] == 0;
}

typedef struct {
  GPtrArray *buttons; /* GBytes *, 8 bytes each */
  GPtrArray *stills;  /* BroCellRef * */
} MenuVob;

static void
menu_vob_free (MenuVob *m)
{
  g_ptr_array_unref (m->buttons);
  g_ptr_array_unref (m->stills);
  g_free (m);
}

typedef struct {
  int vob, cell;
  gint64 first, last;
} Cell;

static int
by_first (gconstpointer a, gconstpointer b)
{
  gint64 x = ((const Cell *) a)->first, y = ((const Cell *) b)->first;
  return x < y ? -1 : x > y;
}

/* One pass over a menu VOB: the button commands of its NAV packs (each once, in order) and a still per VOB/cell id
 * longer than 8 sectors (in menu VOBs of 64 KiB to 512 MiB). */
static MenuVob *
scan_menu_vob (BroByteSource *source, const char *name, gint64 size)
{
  MenuVob *r = g_new0 (MenuVob, 1);
  r->buttons = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  r->stills = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_cell_ref_free);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_bytes_hash, g_bytes_equal, (GDestroyNotify) g_bytes_unref, NULL);
  g_autoptr (GArray) cells = g_array_new (FALSE, FALSE, sizeof (Cell));
  gint64 total = size / SECTOR;
  for (gint64 s = 0; s < total; s += 512) {
    g_autoptr (GBytes) bytes = bro_byte_source_read (source, name, s * SECTOR, (gsize) MIN (512, total - s) * SECTOR);
    View chunk = view_of (bytes);
    if (chunk.n == 0)
      break;
    for (gsize k = 0; k < chunk.n / SECTOR; k++) {
      gint64 o = (gint64) k * SECTOR;
      if (!is_nav (chunk, o))
        continue;
      gint64 pci = o + 0x2D;
      int buttons = MIN (u8 (chunk, pci + 0x60 + 16), 36);
      for (int b = 0; b < buttons; b++) {
        View c = slice_len (chunk, pci + 0x8E + 18 * b + 10, 8);
        GBytes *cmd = g_bytes_new (c.d, c.n);
        if (g_hash_table_add (seen, g_bytes_ref (cmd)))
          g_ptr_array_add (r->buttons, cmd);
        else
          g_bytes_unref (cmd);
      }
      gint64 dsi = o + 0x407;
      int vob = u16 (chunk, dsi + 0x18), cell = u8 (chunk, dsi + 0x1B);
      gint64 at = s + (gint64) k;
      gboolean found = FALSE;
      for (guint i = 0; i < cells->len && !found; i++) {
        Cell *x = &g_array_index (cells, Cell, i);
        if (x->vob == vob && x->cell == cell) {
          x->last = at;
          found = TRUE;
        }
      }
      if (!found) {
        Cell x = { vob, cell, at, at };
        g_array_append_val (cells, x);
      }
    }
  }
  if (size >= 64 * 1024 && size <= 512LL * 1024 * 1024) {
    g_array_sort (cells, by_first);
    for (guint i = 0; i < cells->len; i++) {
      gint64 first = g_array_index (cells, Cell, i).first;
      gint64 end = i + 1 < cells->len ? g_array_index (cells, Cell, i + 1).first : total;
      if (end - first > 8)
        g_ptr_array_add (r->stills, bro_cell_ref_new (name, first, end));
    }
  }
  return r;
}

static gboolean
is_menu_vob (const char *name)
{
  if (strcmp (name, "VIDEO_TS.VOB") == 0)
    return TRUE;
  return strlen (name) == 12 && g_str_has_prefix (name, "VTS_") && g_ascii_isdigit (name[4]) && g_ascii_isdigit (name[5])
         && strcmp (name + 6, "_0.VOB") == 0;
}

static int
by_name (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static void
add_jump (BroNavAnalysis *a, int title, int ptt, const char *how)
{
  for (guint i = 0; i < a->jumps->len; i++) {
    BroNavJump *j = a->jumps->pdata[i];
    if (j->title == title && j->chapter == ptt)
      return;
  }
  g_ptr_array_add (a->jumps, bro_nav_jump_new (title, ptt, how));
}

static GBytes *
read_all (BroByteSource *source, GHashTable *files, const char *name)
{
  gpointer size;
  if (!g_hash_table_lookup_extended (files, name, NULL, &size))
    return g_bytes_new (NULL, 0);
  return bro_byte_source_read (source, name, 0, (gsize) *(gint64 *) size);
}

BroNavAnalysis *
bro_dvd_nav_analyse (BroByteSource *source)
{
  g_autoptr (GPtrArray) list = bro_byte_source_files (source);
  g_autoptr (GHashTable) files = g_hash_table_new (g_str_hash, g_str_equal); /* name → gint64 * (both in list) */
  for (guint i = 0; i < list->len; i++) {
    BroByteFile *f = list->pdata[i];
    g_hash_table_insert (files, f->name, &f->size);
  }
  g_autoptr (GBytes) vmg_bytes = read_all (source, files, "VIDEO_TS.IFO");
  View vmg = view_of (vmg_bytes);
  if (vmg.n <= 0xCC || memcmp (vmg.d, "DVDVIDEO-VMG", 12) != 0)
    return NULL;

  g_autoptr (GPtrArray) names = g_ptr_array_new ();
  for (guint i = 0; i < list->len; i++)
    if (is_menu_vob (((BroByteFile *) list->pdata[i])->name))
      g_ptr_array_add (names, ((BroByteFile *) list->pdata[i])->name);
  g_ptr_array_sort (names, by_name);
  g_autoptr (GHashTable) menus = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) menu_vob_free);
  for (guint i = 0; i < names->len; i++)
    g_hash_table_insert (menus, names->pdata[i], scan_menu_vob (source, names->pdata[i], *(gint64 *) g_hash_table_lookup (files, names->pdata[i])));

  BroNavAnalysis *a = bro_nav_analysis_new ();
  View tt = slice (vmg, (gint64) u32 (vmg, 0xC4) * SECTOR);
  int ntitles = u16 (tt, 0);
  g_autofree int *vts_of = g_new0 (int, ntitles + 1), *ttn_of = g_new0 (int, ntitles + 1);
  for (int i = 0; i < ntitles; i++) {
    vts_of[i + 1] = u8 (tt, 8 + 12 * i + 6);
    ttn_of[i + 1] = u8 (tt, 8 + 12 * i + 7);
  }
  /* The disc title of a VTS title (the last one listed wins). */
#define TITLE_OF(vts, ttn, out)                                                                                         \
  do {                                                                                                                  \
    out = 0;                                                                                                            \
    for (int q = 1; q <= ntitles; q++)                                                                                  \
      if (vts_of[q] == (vts) && ttn_of[q] == (ttn))                                                                     \
        out = q;                                                                                                        \
  } while (0)

  g_autoptr (GArray) vmg_menus = menu_pgcs (vmg, 0xC8);
  for (guint p = 0; p < vmg_menus->len; p++) {
    g_autoptr (GArray) cmds = pgc_commands (g_array_index (vmg_menus, View, p));
    for (guint k = 0; k < cmds->len; k++) {
      View c = g_array_index (cmds, View, k);
      g_autoptr (BroJump) j = bro_dvd_nav_decode_jump (c.d, c.n);
      if (j && j->kind == BRO_JUMP_TITLE)
        add_jump (a, j->number, 1, "menu JumpTT");
    }
  }
  MenuVob *vmg_vob = g_hash_table_lookup (menus, "VIDEO_TS.VOB");
  for (guint k = 0; vmg_vob && k < vmg_vob->buttons->len; k++) {
    gsize n;
    const guint8 *d = g_bytes_get_data (vmg_vob->buttons->pdata[k], &n);
    g_autoptr (BroJump) j = bro_dvd_nav_decode_jump (d, n);
    if (j && j->kind == BRO_JUMP_TITLE)
      add_jump (a, j->number, 1, "button JumpTT");
  }

  for (int vtsn = 0; vtsn <= 255; vtsn++) {
    gboolean used = FALSE;
    for (int q = 1; q <= ntitles && !used; q++)
      used = vts_of[q] == vtsn;
    if (!used)
      continue;
    g_autofree char *name = g_strdup_printf ("VTS_%02d_0.IFO", vtsn);
    if (!g_hash_table_contains (files, name))
      continue;
    g_autoptr (GBytes) ifo_bytes = read_all (source, files, name);
    View ifo = view_of (ifo_bytes);
    View pb = slice (ifo, (gint64) u32 (ifo, 0xC8) * SECTOR);
    int nttu = u16 (pb, 0);
    g_autofree gint64 *offs = g_new0 (gint64, nttu + 1);
    for (int i = 0; i < nttu; i++)
      offs[i] = u32 (pb, 8 + 4 * i);
    offs[nttu] = (gint64) u32 (pb, 4) + 1;
    g_autoptr (GArray) title_pgcs = g_array_new (FALSE, FALSE, sizeof (View));
    add_pgcs (title_pgcs, slice (ifo, (gint64) u32 (ifo, 0xCC) * SECTOR));
    for (int ttn = 1; ttn <= nttu; ttn++) {
      int title;
      TITLE_OF (vtsn, ttn, title);
      if (!title)
        continue;
      BroNavTitle *t = bro_nav_title_new (title);
      int first_pgc = 0;
      for (gint64 j = offs[ttn - 1]; j + 4 <= offs[ttn]; j += 4) {
        int pgcn = u16 (pb, j), pgn = u16 (pb, j + 2);
        if (pgcn < 1 || (guint) pgcn > title_pgcs->len)
          continue;
        if (!first_pgc)
          first_pgc = pgcn;
        View pg = g_array_index (title_pgcs, View, pgcn - 1);
        int nprog = u8 (pg, 2), ncell = u8 (pg, 3);
        int pmo = u16 (pg, 0xE6), cpbo = u16 (pg, 0xE8), cpso = u16 (pg, 0xEA);
        g_autofree int *pmap = g_new0 (int, nprog + 1);
        for (int k = 0; k < nprog; k++)
          pmap[k] = u8 (pg, pmo + k);
        pmap[nprog] = ncell + 1;
        if (pgn < 1 || pgn >= nprog + 1)
          continue;
        double d = 0;
        for (int cell = MAX (0, pmap[pgn - 1] - 1); cell < MAX (0, pmap[pgn] - 1); cell++)
          d += dvd_time (pg, cpbo + 24 * cell + 4);
        double length = NTSC * d;
        int vob = u16 (pg, cpso + 4 * (pmap[pgn - 1] - 1));
        g_array_append_val (t->chapters, length);
        g_array_append_val (t->chapter_vobs, vob);
      }
      for (guint i = 0; i < a->titles->len; i++)
        if (((BroNavTitle *) a->titles->pdata[i])->number == title) {
          g_ptr_array_remove_index (a->titles, i);
          break;
        }
      g_ptr_array_add (a->titles, t);
      if (first_pgc) {
        g_autoptr (GArray) cmds = pgc_commands (g_array_index (title_pgcs, View, first_pgc - 1));
        for (guint k = 0; k < cmds->len; k++) {
          View c = g_array_index (cmds, View, k);
          g_autoptr (BroJump) j = bro_dvd_nav_decode_jump (c.d, c.n);
          if (j && j->kind == BRO_JUMP_PTT && !j->has_title_number) {
            g_autofree char *how = g_strdup_printf ("%sLinkPTTN %d", j->condition, j->chapter);
            add_jump (a, title, j->chapter, how);
          }
        }
      }
    }
    /* Menu commands, then the menu VOB's buttons. */
    g_autoptr (GPtrArray) cmds = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
    g_autoptr (GArray) vts_menus = menu_pgcs (ifo, 0xD0);
    for (guint p = 0; p < vts_menus->len; p++) {
      g_autoptr (GArray) list2 = pgc_commands (g_array_index (vts_menus, View, p));
      for (guint k = 0; k < list2->len; k++) {
        View c = g_array_index (list2, View, k);
        g_ptr_array_add (cmds, g_bytes_new (c.d, c.n));
      }
    }
    g_autofree char *vob_name = g_strdup_printf ("VTS_%02d_0.VOB", vtsn);
    MenuVob *m = g_hash_table_lookup (menus, vob_name);
    for (guint k = 0; m && k < m->buttons->len; k++)
      g_ptr_array_add (cmds, g_bytes_ref (m->buttons->pdata[k]));
    for (guint k = 0; k < cmds->len; k++) {
      gsize n;
      const guint8 *d = g_bytes_get_data (cmds->pdata[k], &n);
      g_autoptr (BroJump) j = bro_dvd_nav_decode_jump (d, n);
      int t = 0;
      if (j && j->kind == BRO_JUMP_PTT && j->has_title_number)
        TITLE_OF (vtsn, j->title_number, t);
      if (t) {
        g_autofree char *how = g_strdup_printf ("menu JumpVTS_PTT %d:%d", j->title_number, j->chapter);
        add_jump (a, t, j->chapter, how);
      }
    }
  }
#undef TITLE_OF
  for (guint i = 0; i < names->len; i++) {
    MenuVob *m = g_hash_table_lookup (menus, names->pdata[i]);
    for (guint k = 0; k < m->stills->len; k++) {
      BroCellRef *c = m->stills->pdata[k];
      g_ptr_array_add (a->stills, bro_cell_ref_new (c->file, c->first_sector, c->end_sector));
    }
  }
  return a;
}

static BroChapterRange
range (GArray *starts, int last_end, int i)
{
  int first = g_array_index (starts, int, i);
  int end = (guint) i + 1 < starts->len ? g_array_index (starts, int, i + 1) - 1 : last_end;
  return (BroChapterRange) { first, MAX (first, end) };
}

static int
by_int (gconstpointer a, gconstpointer b)
{
  int x = *(const int *) a, y = *(const int *) b;
  return x < y ? -1 : x > y;
}

BroEpisodePlan *
bro_dvd_nav_episode_plan (const BroNavAnalysis *analysis, int title_index)
{
  BroNavTitle *t = NULL;
  for (guint i = 0; i < analysis->titles->len && !t; i++)
    if (((BroNavTitle *) analysis->titles->pdata[i])->number == title_index)
      t = analysis->titles->pdata[i];
  gboolean jumped = FALSE;
  for (guint i = 0; i < analysis->jumps->len && !jumped; i++)
    jumped = ((BroNavJump *) analysis->jumps->pdata[i])->title == title_index;
  if (!t || t->chapters->len == 0 || !jumped)
    return NULL;
  int count = (int) t->chapters->len;
  /* chapter → how it is reached (the first way found; chapter 1 is the title start otherwise). */
  g_autoptr (GHashTable) targets = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (guint i = 0; i < analysis->jumps->len; i++) {
    BroNavJump *j = analysis->jumps->pdata[i];
    if (j->title == title_index && !g_hash_table_contains (targets, GINT_TO_POINTER (j->chapter)))
      g_hash_table_insert (targets, GINT_TO_POINTER (j->chapter), j->how);
  }
  if (!g_hash_table_contains (targets, GINT_TO_POINTER (1)))
    g_hash_table_insert (targets, GINT_TO_POINTER (1), (gpointer) "title start");
  GArray *starts = g_array_new (FALSE, FALSE, sizeof (int));
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, targets);
  while (g_hash_table_iter_next (&it, &key, NULL)) {
    int k = GPOINTER_TO_INT (key);
    if (k >= 1 && k <= count)
      g_array_append_val (starts, k);
  }
  g_array_sort (starts, by_int);
  if (starts->len < 2) {
    g_array_unref (starts);
    return NULL;
  }
  double *ch = (double *) t->chapters->data;
  int *vobs = (int *) t->chapter_vobs->data;
  int last = g_array_index (starts, int, starts->len - 1);
  int last_end = last;
  const char *end_rule;
  gboolean several = FALSE;
  for (int i = 1; i < count && !several; i++)
    several = vobs[i] != vobs[0];
  if (several) {
    while (last_end < count && vobs[last_end] == vobs[last - 1])
      last_end++;
    end_rule = "VOB boundary";
  } else {
    last_end = MIN (count, last + (last - g_array_index (starts, int, starts->len - 2)) - 1);
    end_rule = "same chapter count as the previous episode";
  }
  BroEpisodePlan *plan = g_new0 (BroEpisodePlan, 1);
  plan->title = title_index;
  plan->starts = starts;
  plan->last_end = last_end;
  plan->end_rule = g_strdup (end_rule);
  plan->tail = g_array_new (FALSE, FALSE, sizeof (int));
  for (int n = last_end + 1; n <= count; n++)
    if (ch[n - 1] >= 1.0)
      g_array_append_val (plan->tail, n);
  plan->split_chapters = g_array_new (FALSE, FALSE, sizeof (int));
  g_array_append_vals (plan->split_chapters, &g_array_index (starts, int, 1), starts->len - 1);
  if (plan->tail->len)
    g_array_append_val (plan->split_chapters, g_array_index (plan->tail, int, 0));
  for (int n = 1; n <= count; n++)
    if (!(n > last_end && ch[n - 1] < 1.0)) {
      plan->duration += ch[n - 1];
      plan->kept_chapters++;
    }
  plan->chapter_starts = g_array_new (FALSE, FALSE, sizeof (double));
  double acc = 0;
  for (int n = 0; n < count; n++) {
    g_array_append_val (plan->chapter_starts, acc);
    acc += ch[n];
  }
  plan->episode_durations = g_array_new (FALSE, FALSE, sizeof (double));
  plan->reasons = g_new0 (char *, starts->len + 1);
  for (guint i = 0; i < starts->len; i++) {
    BroChapterRange r = range (starts, last_end, (int) i);
    double d = 0;
    for (int n = r.first; n <= r.last; n++)
      d += ch[n - 1];
    g_array_append_val (plan->episode_durations, d);
    plan->reasons[i] = g_strdup (g_hash_table_lookup (targets, GINT_TO_POINTER (g_array_index (starts, int, i))));
  }
  return plan;
}

static int
by_episodes (gconstpointer a, gconstpointer b)
{
  const BroEpisodePlan *x = *(const BroEpisodePlan *const *) a, *y = *(const BroEpisodePlan *const *) b;
  if (x->starts->len != y->starts->len)
    return x->starts->len > y->starts->len ? -1 : 1;
  return x->title < y->title ? -1 : x->title > y->title;
}

GPtrArray *
bro_dvd_nav_plans (const BroNavAnalysis *analysis)
{
  GPtrArray *plans = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_episode_plan_free);
  g_autoptr (GArray) done = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < analysis->jumps->len; i++) {
    int title = ((BroNavJump *) analysis->jumps->pdata[i])->title;
    gboolean seen = FALSE;
    for (guint k = 0; k < done->len && !seen; k++)
      seen = g_array_index (done, int, k) == title;
    if (seen)
      continue;
    g_array_append_val (done, title);
    BroEpisodePlan *p = bro_dvd_nav_episode_plan (analysis, title);
    if (p)
      g_ptr_array_add (plans, p);
  }
  g_ptr_array_sort (plans, by_episodes);
  return plans;
}

GPtrArray *
bro_dvd_nav_still_cells (const BroNavAnalysis *analysis)
{
  return analysis->stills;
}

BroChapterRange
bro_dvd_nav_chapter_range (const BroEpisodePlan *plan, int episode)
{
  return range (plan->starts, plan->last_end, episode);
}

gboolean
bro_dvd_nav_is_plausible (const BroEpisodePlan *plan, gboolean strict)
{
  if ((int) plan->starts->len < (strict ? 3 : 2) || plan->episode_durations->len == 0)
    return FALSE;
  double lo = INFINITY, hi = -INFINITY;
  for (guint i = 0; i < plan->episode_durations->len; i++) {
    double d = g_array_index (plan->episode_durations, double, i);
    lo = MIN (lo, d);
    hi = MAX (hi, d);
  }
  if (lo <= 0)
    return FALSE;
  return strict ? lo >= 900 && hi / lo <= 1.35 : lo >= 300 && hi / lo <= 2.0;
}

gboolean
bro_dvd_nav_matches_chapter_count (const BroEpisodePlan *plan, int chapters)
{
  return chapters >= plan->kept_chapters - 1 && chapters <= (int) plan->chapter_starts->len;
}

GArray *
bro_dvd_nav_mkv_chapters (GArray *split, const BroEpisodePlan *plan, GArray *mkv_starts)
{
  GArray *result = g_array_new (FALSE, FALSE, sizeof (int));
  if (!mkv_starts || mkv_starts->len == 0) {
    g_array_append_vals (result, split->data, split->len);
    return result;
  }
  for (guint i = 0; i < split->len; i++) {
    double want = g_array_index (plan->chapter_starts, double, g_array_index (split, int, i) - 1);
    guint best = 0;
    for (guint k = 1; k < mkv_starts->len; k++)
      if (fabs (g_array_index (mkv_starts, double, k) - want) < fabs (g_array_index (mkv_starts, double, best) - want))
        best = k;
    if (fabs (g_array_index (mkv_starts, double, best) - want) > 1.0) {
      g_array_unref (result);
      return NULL;
    }
    int n = (int) best + 1;
    g_array_append_val (result, n);
  }
  return result;
}
