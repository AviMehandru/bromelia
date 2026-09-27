/* bro-logic.c — title selection rules, templates and argument splitting. */
#include "bro-logic.h"

#include <stdlib.h>
#include <string.h>

/* ---- index patterns ---- */

typedef enum { ITEM_SINGLE, ITEM_RANGE, ITEM_OPEN, ITEM_LAST, ITEM_ALL } ItemKind;
typedef struct { ItemKind kind; int lo, hi; } PatternItem;

static gboolean
parse_nonneg (const char *s, int *out)
{
  char *end;
  long v;
  if (!*s)
    return FALSE;
  v = strtol (s, &end, 10);
  if (*end || v < 0)
    return FALSE;
  *out = (int) v;
  return TRUE;
}

static GArray *
parse_pattern (const char *pattern, char **error)
{
  GArray *items = g_array_new (FALSE, FALSE, sizeof (PatternItem));
  g_auto (GStrv) parts = g_strsplit_set (pattern ? pattern : "", ",; ", -1);
  for (int i = 0; parts[i]; i++)
    {
      g_autofree char *p = g_ascii_strdown (g_strstrip (parts[i]), -1);
      PatternItem it = { 0 };
      char *dash;
      if (!*p)
        continue;
      if (g_str_equal (p, "*") || g_str_equal (p, "all"))
        it.kind = ITEM_ALL;
      else if (g_str_equal (p, "last"))
        it.kind = ITEM_LAST;
      else if ((dash = strchr (p, '-')))
        {
          *dash = '\0';
          if (!parse_nonneg (p, &it.lo))
            goto bad;
          if (dash[1] == '\0')
            it.kind = ITEM_OPEN;
          else if (parse_nonneg (dash + 1, &it.hi) && it.hi >= it.lo)
            it.kind = ITEM_RANGE;
          else
            goto bad;
        }
      else if (parse_nonneg (p, &it.lo))
        it.kind = ITEM_SINGLE;
      else
        goto bad;
      g_array_append_val (items, it);
      continue;
bad:
      if (error)
        *error = g_strdup_printf ("Invalid index pattern element “%s”", parts[i]);
      g_array_unref (items);
      return NULL;
    }
  return items;
}

gboolean
bro_index_pattern_validate (const char *pattern, char **error)
{
  GArray *items = parse_pattern (pattern, error);
  if (!items)
    return FALSE;
  g_array_unref (items);
  return TRUE;
}

static gboolean
items_match (GArray *items, int v, int max)
{
  for (guint i = 0; i < items->len; i++)
    {
      PatternItem *it = &g_array_index (items, PatternItem, i);
      switch (it->kind)
        {
        case ITEM_ALL: return TRUE;
        case ITEM_LAST: if (v == max) return TRUE; break;
        case ITEM_SINGLE: if (v == it->lo) return TRUE; break;
        case ITEM_RANGE: if (v >= it->lo && v <= it->hi) return TRUE; break;
        case ITEM_OPEN: if (v >= it->lo) return TRUE; break;
        }
    }
  return FALSE;
}

gboolean
bro_index_pattern_matches (const char *pattern, int value, int max_value)
{
  GArray *items = parse_pattern (pattern, NULL);
  gboolean r;
  if (!items)
    return FALSE;
  r = items_match (items, value, max_value);
  g_array_unref (items);
  return r;
}

/* ---- title selection ---- */

static void
decision_clear (BroTitleDecision *d)
{
  g_free (d->reason);
}

void
bro_selection_result_free (BroSelectionResult *r)
{
  if (!r)
    return;
  g_array_unref (r->decisions);
  g_free (r->error);
  g_free (r);
}

static char *
match_text (BroTitle *t)
{
  GString *s = g_string_new (NULL);
  const int ids[] = { BRO_ATTR_NAME, BRO_ATTR_COMMENT, BRO_ATTR_OUTPUT_FILE_NAME, BRO_ATTR_SOURCE_FILE_NAME, BRO_ATTR_SEGMENTS_MAP };
  for (guint i = 0; i < G_N_ELEMENTS (ids); i++)
    {
      const char *v = bro_title_attr (t, ids[i]);
      if (v && *v)
        g_string_append_printf (s, "%s%s", s->len ? " " : "", v);
    }
  if (bro_title_source_id (t) >= 0)
    g_string_append_printf (s, "%s#%d", s->len ? " " : "", bro_title_source_id (t));
  if (bro_title_attr (t, BRO_ATTR_DURATION))
    g_string_append_printf (s, "%s%s", s->len ? " " : "", bro_title_attr (t, BRO_ATTR_DURATION));
  return g_string_free (s, FALSE);
}

typedef struct {
  GHashTable *reasons; /* title index -> reason (only first rejection counts) */
} Rejections;

static void
reject (GHashTable *reasons, BroTitle *t, char *why)
{
  if (!g_hash_table_contains (reasons, GINT_TO_POINTER (t->index)))
    g_hash_table_insert (reasons, GINT_TO_POINTER (t->index), why);
  else
    g_free (why);
}

static int
rank_longest (gconstpointer a, gconstpointer b)
{
  BroTitle *x = *(BroTitle **) a, *y = *(BroTitle **) b;
  int dx = bro_title_duration (x), dy = bro_title_duration (y);
  if (dx != dy) return dy - dx;
  if (bro_title_chapters (x) != bro_title_chapters (y)) return bro_title_chapters (y) - bro_title_chapters (x);
  if (bro_title_size (x) != bro_title_size (y)) return bro_title_size (y) > bro_title_size (x) ? 1 : -1;
  return x->index - y->index;
}

static int
by_index (gconstpointer a, gconstpointer b)
{
  return (*(BroTitle **) a)->index - (*(BroTitle **) b)->index;
}

BroSelectionResult *
bro_select_titles (BroDiscInfo *info, const BroTitleSelection *rule)
{
  BroSelectionResult *res = g_new0 (BroSelectionResult, 1);
  g_autoptr (GHashTable) reasons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  g_autoptr (GHashTable) chosen_set = g_hash_table_new (g_direct_hash, g_direct_equal);
  g_autoptr (GPtrArray) candidates = g_ptr_array_new ();
  g_autoptr (GPtrArray) chosen = g_ptr_array_new ();
  g_autoptr (GRegex) include = NULL;
  g_autoptr (GRegex) exclude = NULL;

  res->decisions = g_array_new (FALSE, TRUE, sizeof (BroTitleDecision));
  g_array_set_clear_func (res->decisions, (GDestroyNotify) decision_clear);

  if (rule->include_pattern && *rule->include_pattern)
    {
      include = g_regex_new (rule->include_pattern, G_REGEX_CASELESS, 0, NULL);
      if (!include)
        res->error = g_strdup ("Invalid include pattern");
    }
  if (rule->exclude_pattern && *rule->exclude_pattern)
    {
      exclude = g_regex_new (rule->exclude_pattern, G_REGEX_CASELESS, 0, NULL);
      if (!exclude)
        res->error = g_strdup ("Invalid exclude pattern");
    }

  /* 1. Filters. */
  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      int d = bro_title_duration (t);
      gint64 mb = bro_title_size (t) / 1000000;
      g_autofree char *hay = match_text (t);
      if (rule->min_duration_seconds > 0 && d < rule->min_duration_seconds)
        { g_autofree char *f = bro_format_duration (rule->min_duration_seconds); reject (reasons, t, g_strdup_printf ("Shorter than %s", f)); continue; }
      if (rule->max_duration_seconds > 0 && d > rule->max_duration_seconds)
        { g_autofree char *f = bro_format_duration (rule->max_duration_seconds); reject (reasons, t, g_strdup_printf ("Longer than %s", f)); continue; }
      if (rule->min_chapters > 0 && bro_title_chapters (t) < rule->min_chapters)
        { reject (reasons, t, g_strdup_printf ("Fewer than %d chapters", rule->min_chapters)); continue; }
      if (rule->max_chapters > 0 && bro_title_chapters (t) > rule->max_chapters)
        { reject (reasons, t, g_strdup_printf ("More than %d chapters", rule->max_chapters)); continue; }
      if (rule->min_size_mb > 0 && mb < rule->min_size_mb)
        { reject (reasons, t, g_strdup_printf ("Smaller than %d MB", rule->min_size_mb)); continue; }
      if (rule->max_size_mb > 0 && mb > rule->max_size_mb)
        { reject (reasons, t, g_strdup_printf ("Larger than %d MB", rule->max_size_mb)); continue; }
      if (include && !g_regex_match (include, hay, 0, NULL))
        { reject (reasons, t, g_strdup ("Does not match include pattern")); continue; }
      if (exclude && g_regex_match (exclude, hay, 0, NULL))
        { reject (reasons, t, g_strdup ("Matches exclude pattern")); continue; }
      if (rule->skip_alternate_angles && bro_title_angle (t) > 1)
        { reject (reasons, t, g_strdup_printf ("Alternate angle %d", bro_title_angle (t))); continue; }
      g_ptr_array_add (candidates, t);
    }

  /* 2. Duplicates. */
  if (rule->skip_duplicates)
    {
      g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      g_autoptr (GPtrArray) kept = g_ptr_array_new ();
      for (guint i = 0; i < candidates->len; i++)
        {
          BroTitle *t = candidates->pdata[i];
          const char *seg = bro_title_str (t, BRO_ATTR_SEGMENTS_MAP);
          char *key;
          gpointer first;
          if (!*seg)
            {
              g_ptr_array_add (kept, t);
              continue;
            }
          key = g_strdup_printf ("%s|%d|%d", seg, bro_title_duration (t), MAX (bro_title_angle (t), 0));
          if (g_hash_table_lookup_extended (seen, key, NULL, &first))
            {
              reject (reasons, t, g_strdup_printf ("Duplicate of title %d", GPOINTER_TO_INT (first)));
              g_free (key);
              continue;
            }
          g_hash_table_insert (seen, key, GINT_TO_POINTER (t->index));
          g_ptr_array_add (kept, t);
        }
      g_ptr_array_set_size (candidates, 0);
      g_ptr_array_extend (candidates, kept, NULL, NULL);
    }

  /* 3. Strategy. */
  switch (rule->strategy)
    {
    case BRO_STRATEGY_ALL:
      g_ptr_array_extend (chosen, candidates, NULL, NULL);
      break;
    case BRO_STRATEGY_LONGEST:
      {
        int n = MAX (1, rule->longest_count);
        g_autoptr (GPtrArray) ranked = g_ptr_array_copy (candidates, NULL, NULL);
        g_ptr_array_sort (ranked, rank_longest);
        for (guint i = 0; i < ranked->len; i++)
          {
            if ((int) i < n)
              g_ptr_array_add (chosen, ranked->pdata[i]);
            else
              reject (reasons, ranked->pdata[i], g_strdup_printf ("Not among the %d longest", n));
          }
      }
      break;
    case BRO_STRATEGY_INDICES:
      {
        char *err = NULL;
        GArray *items = parse_pattern (rule->index_pattern, &err);
        if (!items)
          {
            g_free (res->error);
            res->error = err;
            for (guint i = 0; i < candidates->len; i++)
              reject (reasons, candidates->pdata[i], g_strdup ("Invalid index pattern"));
            break;
          }
        int max = 0;
        for (guint i = 0; i < info->titles->len; i++)
          {
            BroTitle *t = info->titles->pdata[i];
            int v = rule->index_base == BRO_INDEX_MAKEMKV ? t->index : bro_title_source_id (t);
            max = MAX (max, v);
          }
        for (guint i = 0; i < candidates->len; i++)
          {
            BroTitle *t = candidates->pdata[i];
            int v = rule->index_base == BRO_INDEX_MAKEMKV ? t->index : bro_title_source_id (t);
            if (items_match (items, v, max))
              g_ptr_array_add (chosen, t);
            else
              reject (reasons, t, g_strdup ("Not in index pattern"));
          }
        g_array_unref (items);
      }
      break;
    case BRO_STRATEGY_MANUAL:
      res->requires_manual = TRUE;
      for (guint i = 0; i < candidates->len; i++)
        reject (reasons, candidates->pdata[i], g_strdup ("Choose manually"));
      break;
    }

  g_ptr_array_sort (chosen, by_index);
  if (rule->max_titles > 0 && (int) chosen->len > rule->max_titles)
    {
      for (guint i = rule->max_titles; i < chosen->len; i++)
        reject (reasons, chosen->pdata[i], g_strdup_printf ("Over the limit of %d titles", rule->max_titles));
      g_ptr_array_set_size (chosen, rule->max_titles);
    }
  for (guint i = 0; i < chosen->len; i++)
    g_hash_table_add (chosen_set, GINT_TO_POINTER (((BroTitle *) chosen->pdata[i])->index));

  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      BroTitleDecision d = { t->index, FALSE, NULL };
      if (g_hash_table_contains (chosen_set, GINT_TO_POINTER (t->index)))
        {
          d.selected = TRUE;
          d.reason = g_strdup ("Selected");
        }
      else
        {
          const char *why = g_hash_table_lookup (reasons, GINT_TO_POINTER (t->index));
          d.reason = g_strdup (why ? why : "Excluded");
        }
      g_array_append_val (res->decisions, d);
    }
  return res;
}

GArray *
bro_selection_result_selected (BroSelectionResult *r)
{
  GArray *a = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < r->decisions->len; i++)
    {
      BroTitleDecision *d = &g_array_index (r->decisions, BroTitleDecision, i);
      if (d->selected)
        g_array_append_val (a, d->title_index);
    }
  return a;
}

gboolean
bro_selection_result_is_selected (BroSelectionResult *r, int title)
{
  for (guint i = 0; i < r->decisions->len; i++)
    {
      BroTitleDecision *d = &g_array_index (r->decisions, BroTitleDecision, i);
      if (d->title_index == title)
        return d->selected;
    }
  return FALSE;
}

const char *
bro_selection_result_reason (BroSelectionResult *r, int title)
{
  for (guint i = 0; i < r->decisions->len; i++)
    {
      BroTitleDecision *d = &g_array_index (r->decisions, BroTitleDecision, i);
      if (d->title_index == title)
        return d->reason;
    }
  return "";
}

/* ---- templates ---- */

const BroTokenHelp bro_folder_tokens[] = {
  { "disc", "Disc name (falls back to the volume label)" }, { "volume", "Volume label" },
  { "type", "dvd, bd, hddvd or disc" }, { "drive", "Name of the drive configuration" },
  { "date", "Date, yyyy-MM-dd" }, { "time", "Time, HH-mm-ss" }, { "year", "Year" }, { "month", "Month" }, { "day", "Day" },
  { "job", "Short job identifier" }, { NULL, NULL } };

const BroTokenHelp bro_file_tokens[] = {
  { "disc", "Disc name (falls back to the volume label)" }, { "volume", "Volume label" },
  { "type", "dvd, bd, hddvd or disc" }, { "drive", "Name of the drive configuration" },
  { "date", "Date, yyyy-MM-dd" }, { "time", "Time, HH-mm-ss" }, { "year", "Year" }, { "month", "Month" }, { "day", "Day" },
  { "job", "Short job identifier" },
  { "title", "Title name (or the disc name when the title has none)" }, { "index", "MakeMKV title number (0-based)" },
  { "n", "Position of the title in this job (1-based)" }, { "source", "Source title ID (playlist / VTS number)" },
  { "duration", "Duration, h-mm-ss" }, { "chapters", "Chapter count" },
  { "original", "MakeMKV's original file name without extension" }, { "comment", "Title comment reported by MakeMKV" },
  { NULL, NULL } };

const BroTokenHelp bro_script_tokens[] = {
  { "disc", "Disc name" }, { "volume", "Volume label" }, { "type", "dvd, bd, hddvd or disc" },
  { "drive", "Name of the drive configuration" }, { "date", "Date" }, { "time", "Time" }, { "job", "Short job identifier" },
  { "outputDir", "Job output folder" }, { "file", "Current file (per-file steps) or first file" },
  { "filename", "Current file name (per-file steps)" }, { "stem", "Current file name without extension (per-file steps)" },
  { "files", "All produced files (a lone {files} argument expands to one argument per file)" },
  { "status", "success, failed or cancelled" }, { "manifest", "Path of the job manifest JSON" },
  { "device", "OS device of the drive" }, { NULL, NULL } };

char *
bro_sanitize_component (const char *s)
{
  GString *out = g_string_new (NULL);
  const char *p;
  for (p = s ? s : ""; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (strchr ("/\\:*?\"<>|", (int) c) && c < 128)
        g_string_append_c (out, '-');
      else if (g_unichar_iscntrl (c))
        g_string_append_c (out, '-');
      else
        g_string_append_unichar (out, c);
    }
  /* Trim spaces and dots at both ends. */
  gsize start = 0, end = out->len;
  while (start < end && (out->str[start] == ' ' || out->str[start] == '.'))
    start++;
  while (end > start && (out->str[end - 1] == ' ' || out->str[end - 1] == '.'))
    end--;
  char *r = g_strndup (out->str + start, end - start);
  g_string_free (out, TRUE);
  return r;
}

static const char *
matching_brace (const char *open)
{
  int depth = 0;
  for (const char *p = open; *p; p++)
    {
      if (*p == '{')
        depth++;
      else if (*p == '}' && --depth == 0)
        return p;
    }
  return NULL;
}

static char *
expand (const char *inner, GHashTable *values, gboolean sanitize)
{
  const char *q = strchr (inner, '?');
  const char *v;
  if (q)
    {
      g_autofree char *key = g_strndup (inner, q - inner);
      v = g_hash_table_lookup (values, key);
      if (!v)
        return NULL;
      return *v ? bro_template_render (q + 1, values, sanitize) : g_strdup ("");
    }
  const char *colon = strchr (inner, ':');
  g_autofree char *key = colon ? g_strndup (inner, colon - inner) : g_strdup (inner);
  int pad = colon ? atoi (colon + 1) : 0;
  v = g_hash_table_lookup (values, key);
  if (!v)
    return NULL;
  char *value;
  char *end;
  long n = strtol (v, &end, 10);
  if (pad > 0 && *v && !*end && n >= 0)
    value = g_strdup_printf ("%0*ld", pad, n);
  else
    value = g_strdup (v);
  if (sanitize)
    {
      char *s = bro_sanitize_component (value);
      g_free (value);
      value = s;
    }
  return value;
}

char *
bro_template_render (const char *tmpl, GHashTable *values, gboolean sanitize)
{
  GString *out = g_string_new (NULL);
  const char *p = tmpl ? tmpl : "";
  while (*p)
    {
      if (*p == '{')
        {
          const char *close = matching_brace (p);
          if (close)
            {
              g_autofree char *inner = g_strndup (p + 1, close - p - 1);
              g_autofree char *rep = expand (inner, values, sanitize);
              if (rep)
                g_string_append (out, rep);
              else
                g_string_append_len (out, p, close - p + 1);
              p = close + 1;
              continue;
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}

char *
bro_template_render_path (const char *tmpl, GHashTable *values)
{
  g_autofree char *slashed = g_strdup (tmpl ? tmpl : "");
  g_strdelimit (slashed, "\\", '/');
  g_autofree char *rendered = bro_template_render (slashed, values, TRUE);
  g_auto (GStrv) parts = g_strsplit (rendered, "/", -1);
  GString *out = g_string_new (NULL);
  for (int i = 0; parts[i]; i++)
    {
      g_autofree char *c = bro_sanitize_component (parts[i]);
      if (!*c || g_str_equal (c, ".."))
        continue;
      g_string_append_printf (out, "%s%s", out->len ? G_DIR_SEPARATOR_S : "", c);
    }
  return g_string_free (out, FALSE);
}

GHashTable *
bro_template_values_new (void)
{
  GHashTable *v = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_hash_table_insert (v, g_strdup ("date"), g_date_time_format (now, "%Y-%m-%d"));
  g_hash_table_insert (v, g_strdup ("time"), g_date_time_format (now, "%H-%M-%S"));
  g_hash_table_insert (v, g_strdup ("year"), g_date_time_format (now, "%Y"));
  g_hash_table_insert (v, g_strdup ("month"), g_date_time_format (now, "%m"));
  g_hash_table_insert (v, g_strdup ("day"), g_date_time_format (now, "%d"));
  return v;
}

void
bro_template_values_set (GHashTable *values, const char *key, const char *value)
{
  g_hash_table_replace (values, g_strdup (key), g_strdup (value ? value : ""));
}

/* ---- arguments ---- */

GPtrArray *
bro_split_arguments (const char *s)
{
  GPtrArray *args = g_ptr_array_new_with_free_func (g_free);
  GString *cur = g_string_new (NULL);
  gboolean single = FALSE, dbl = FALSE, has = FALSE;
  for (const char *p = s ? s : ""; *p; p++)
    {
      char c = *p;
      if (single)
        {
          if (c == '\'') single = FALSE; else g_string_append_c (cur, c);
        }
      else if (dbl)
        {
          if (c == '"')
            dbl = FALSE;
          else if (c == '\\' && p[1])
            {
              char n = *++p;
              if (n == '"' || n == '\\' || n == '$' || n == '`')
                g_string_append_c (cur, n);
              else
                {
                  g_string_append_c (cur, c);
                  g_string_append_c (cur, n);
                }
            }
          else
            g_string_append_c (cur, c);
        }
      else if (c == '\'')
        single = has = TRUE;
      else if (c == '"')
        dbl = has = TRUE;
      else if (c == '\\' && p[1])
        {
          g_string_append_c (cur, *++p);
          has = TRUE;
        }
      else if (c == ' ' || c == '\t' || c == '\n')
        {
          if (has || cur->len)
            {
              g_ptr_array_add (args, g_strdup (cur->str));
              g_string_truncate (cur, 0);
              has = FALSE;
            }
        }
      else
        {
          g_string_append_c (cur, c);
          has = TRUE;
        }
    }
  if (has || cur->len)
    g_ptr_array_add (args, g_strdup (cur->str));
  g_string_free (cur, TRUE);
  return args;
}

char *
bro_quote_argument (const char *a)
{
  gboolean plain = *a != '\0';
  for (const char *p = a; *p && plain; p++)
    if (!(g_ascii_isalnum (*p) || strchr ("-_./:=+,@%", *p)))
      plain = FALSE;
  return plain ? g_strdup (a) : g_shell_quote (a);
}

char *
bro_command_line (const char *const *argv)
{
  GString *s = g_string_new (NULL);
  for (int i = 0; argv[i]; i++)
    {
      g_autofree char *q = bro_quote_argument (argv[i]);
      g_string_append_printf (s, "%s%s", i ? " " : "", q);
    }
  return g_string_free (s, FALSE);
}
