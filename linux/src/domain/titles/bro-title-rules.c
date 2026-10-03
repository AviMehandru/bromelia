/* bro-title-rules.c */
#include "bro-title-rules.h"
#include "bro-duration.h"
#include "bro-robot-private.h"

#include <limits.h>
#include <string.h>

typedef struct {
  gboolean set;
  gboolean selected;
  BroBroMessage *reason;
} Reason;

typedef struct {
  GArray *lows, *highs; /* int; high INT_MAX = open */
  gboolean all, last;
} IndexPattern;

static void
index_pattern_clear (IndexPattern *p)
{
  g_array_unref (p->lows);
  g_array_unref (p->highs);
}

/* Index patterns such as "0,2-4,7-", "last", "all" or "*". FALSE when invalid. */
static gboolean
index_pattern_parse (const char *text, IndexPattern *p)
{
  p->lows = g_array_new (FALSE, FALSE, sizeof (int));
  p->highs = g_array_new (FALSE, FALSE, sizeof (int));
  p->all = p->last = FALSE;
  g_auto (GStrv) parts = g_strsplit_set (text, ",; ", -1);
  for (int i = 0; parts[i]; i++) {
    if (!parts[i][0])
      continue;
    g_autofree char *part = g_ascii_strdown (g_strstrip (parts[i]), -1);
    if (strcmp (part, "*") == 0 || strcmp (part, "all") == 0) {
      p->all = TRUE;
      continue;
    }
    if (strcmp (part, "last") == 0) {
      p->last = TRUE;
      continue;
    }
    int low, high;
    char *dash = strchr (part, '-');
    if (dash) {
      g_autofree char *lo = g_strndup (part, dash - part);
      if (!_bro_robot_parse_int (lo, &low))
        return FALSE;
      if (dash[1] == '\0') {
        high = INT_MAX;
      } else if (!_bro_robot_parse_int (dash + 1, &high) || high < low) {
        return FALSE;
      }
    } else {
      if (!_bro_robot_parse_int (part, &low) || low < 0)
        return FALSE;
      high = low;
    }
    g_array_append_val (p->lows, low);
    g_array_append_val (p->highs, high);
  }
  return TRUE;
}

static gboolean
index_pattern_matches (const IndexPattern *p, int value, int max_value)
{
  if (p->all || (p->last && value == max_value))
    return TRUE;
  for (guint i = 0; i < p->lows->len; i++)
    if (value >= g_array_index (p->lows, int, i) && value <= g_array_index (p->highs, int, i))
      return TRUE;
  return FALSE;
}

static BroBroMessage *
m0 (BroMessageCode code)
{
  return bro_bro_message_new (code, NULL, BRO_SEVERITY_INFO);
}

static BroBroMessage *
m1 (BroMessageCode code, const char *key, BroJsonValue *value)
{
  BroJsonValue *p = bro_json_value_new_object ();
  bro_json_value_set (p, key, value);
  return bro_bro_message_new (code, p, BRO_SEVERITY_INFO);
}

static BroJsonValue *
clock_value (int seconds)
{
  BroDuration d = { seconds };
  g_autofree char *text = bro_duration_format_clock (d);
  return bro_json_value_new_string (text);
}

/* The text include and exclude patterns are matched against: name, comment, output file name, source file
 * name, segment map, #<source id> and duration. */
static char *
match_text (const BroTitle *t)
{
  g_autofree char *id = t->has_source_title_id ? g_strdup_printf ("#%d", t->source_title_id) : g_strdup ("");
  const char *parts[] = { t->name, t->comment, t->output_file_name, t->source_file, t->segment_map, id, t->duration };
  GString *s = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (parts); i++) {
    if (!parts[i][0])
      continue;
    if (s->len)
      g_string_append_c (s, ' ');
    g_string_append (s, parts[i]);
  }
  return g_string_free (s, FALSE);
}

static int
by_index (gconstpointer a, gconstpointer b)
{
  const BroTitle *x = *(const BroTitle **) a, *y = *(const BroTitle **) b;
  return x->index < y->index ? -1 : x->index > y->index;
}

static int
by_length (gconstpointer a, gconstpointer b)
{
  const BroTitle *x = *(const BroTitle **) a, *y = *(const BroTitle **) b;
  if (x->duration_seconds != y->duration_seconds)
    return x->duration_seconds > y->duration_seconds ? -1 : 1;
  if (x->chapters != y->chapters)
    return x->chapters > y->chapters ? -1 : 1;
  if (x->size_bytes != y->size_bytes)
    return x->size_bytes > y->size_bytes ? -1 : 1;
  return by_index (a, b);
}

BroSelection *
bro_title_rules_select (const BroListing *listing, const BroTitleSettings *rules)
{
  GPtrArray *titles = listing->titles;
  g_autoptr (GHashTable) reasons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
#define SET_REASON(t, sel, msg)                                                                                        \
  do {                                                                                                                 \
    Reason *r_ = g_hash_table_lookup (reasons, GINT_TO_POINTER ((t)->index));                                         \
    if (!r_) {                                                                                                         \
      r_ = g_new0 (Reason, 1);                                                                                         \
      g_hash_table_insert (reasons, GINT_TO_POINTER ((t)->index), r_);                                                \
    }                                                                                                                  \
    bro_bro_message_free (r_->reason);                                                                                 \
    r_->set = TRUE;                                                                                                    \
    r_->selected = (sel);                                                                                              \
    r_->reason = (msg);                                                                                                \
  } while (0)
#define REJECT(t, msg)                                                                                                 \
  do {                                                                                                                 \
    BroBroMessage *m_ = (msg);                                                                                         \
    if (g_hash_table_contains (reasons, GINT_TO_POINTER ((t)->index)))                                                \
      bro_bro_message_free (m_);                                                                                       \
    else                                                                                                               \
      SET_REASON (t, FALSE, m_);                                                                                       \
  } while (0)

  BroSelection *sel = g_new0 (BroSelection, 1);
  sel->indices = g_array_new (FALSE, FALSE, sizeof (int));
  sel->trace = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_selection_trace_free);

  g_autoptr (GRegex) include = NULL;
  g_autoptr (GRegex) exclude = NULL;
  if (rules->include_pattern[0]) {
    include = g_regex_new (rules->include_pattern, G_REGEX_CASELESS, 0, NULL);
    if (!include)
      sel->error = bro_bro_message_new (BRO_MSG_TITLES_INVALID_INCLUDE, NULL, BRO_SEVERITY_ERROR);
  }
  if (rules->exclude_pattern[0]) {
    exclude = g_regex_new (rules->exclude_pattern, G_REGEX_CASELESS, 0, NULL);
    if (!exclude) {
      bro_bro_message_free (sel->error);
      sel->error = bro_bro_message_new (BRO_MSG_TITLES_INVALID_EXCLUDE, NULL, BRO_SEVERITY_ERROR);
    }
  }

  /* Not g_ptr_array_copy: the copy would free the listing's titles. */
  g_autoptr (GPtrArray) sorted = g_ptr_array_new ();
  g_ptr_array_extend (sorted, titles, NULL, NULL);
  g_ptr_array_sort (sorted, by_index);
  g_autoptr (GPtrArray) candidates = g_ptr_array_new ();
  for (guint i = 0; i < sorted->len; i++) {
    const BroTitle *t = sorted->pdata[i];
    int d = t->duration_seconds;
    gint64 mb = t->size_bytes / 1000000;
    if (rules->min_duration_seconds > 0 && d < rules->min_duration_seconds) {
      REJECT (t, m1 (BRO_MSG_TITLES_REASON_SHORTER_THAN, "duration", clock_value (rules->min_duration_seconds)));
      continue;
    }
    if (rules->max_duration_seconds > 0 && d > rules->max_duration_seconds) {
      REJECT (t, m1 (BRO_MSG_TITLES_REASON_LONGER_THAN, "duration", clock_value (rules->max_duration_seconds)));
      continue;
    }
    if (rules->min_chapters > 0 && t->chapters < rules->min_chapters) {
      REJECT (t, m1 (BRO_MSG_TITLES_REASON_FEWER_CHAPTERS, "count", bro_json_value_new_integer (rules->min_chapters)));
      continue;
    }
    if (rules->max_chapters > 0 && t->chapters > rules->max_chapters) {
      REJECT (t, m1 (BRO_MSG_TITLES_REASON_MORE_CHAPTERS, "count", bro_json_value_new_integer (rules->max_chapters)));
      continue;
    }
    if (rules->min_size_mb > 0 && mb < rules->min_size_mb) {
      REJECT (t, m1 (BRO_MSG_TITLES_REASON_SMALLER_THAN, "mb", bro_json_value_new_integer (rules->min_size_mb)));
      continue;
    }
    if (rules->max_size_mb > 0 && mb > rules->max_size_mb) {
      REJECT (t, m1 (BRO_MSG_TITLES_REASON_LARGER_THAN, "mb", bro_json_value_new_integer (rules->max_size_mb)));
      continue;
    }
    g_autofree char *hay = match_text (t);
    if (include && !g_regex_match (include, hay, 0, NULL)) {
      REJECT (t, m0 (BRO_MSG_TITLES_REASON_NO_INCLUDE_MATCH));
      continue;
    }
    if (exclude && g_regex_match (exclude, hay, 0, NULL)) {
      REJECT (t, m0 (BRO_MSG_TITLES_REASON_EXCLUDE_MATCH));
      continue;
    }
    if (rules->skip_alternate_angles && t->has_angle && t->angle > 1) {
      REJECT (t, m1 (BRO_MSG_TITLES_REASON_ALTERNATE_ANGLE, "angle", bro_json_value_new_integer (t->angle)));
      continue;
    }
    g_ptr_array_add (candidates, (gpointer) t);
  }

  if (rules->skip_duplicates) {
    g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr (GPtrArray) kept = g_ptr_array_new ();
    for (guint i = 0; i < candidates->len; i++) {
      const BroTitle *t = candidates->pdata[i];
      if (!t->segment_map[0]) {
        g_ptr_array_add (kept, (gpointer) t);
        continue;
      }
      char *key = g_strdup_printf ("%s|%d|%d", t->segment_map, t->duration_seconds, t->has_angle ? t->angle : 0);
      gpointer first;
      if (g_hash_table_lookup_extended (seen, key, NULL, &first)) {
        REJECT (t, m1 (BRO_MSG_TITLES_REASON_DUPLICATE_OF, "title", bro_json_value_new_integer (GPOINTER_TO_INT (first))));
        g_free (key);
        continue;
      }
      g_hash_table_insert (seen, key, GINT_TO_POINTER (t->index));
      g_ptr_array_add (kept, (gpointer) t);
    }
    g_ptr_array_set_size (candidates, 0);
    g_ptr_array_extend (candidates, kept, NULL, NULL);
  }

  g_autoptr (GPtrArray) chosen = g_ptr_array_new ();
  switch (rules->strategy) {
  case BRO_TITLE_STRATEGY_ALL:
    g_ptr_array_extend (chosen, candidates, NULL, NULL);
    break;
  case BRO_TITLE_STRATEGY_LONGEST: {
    guint n = MAX (1, rules->longest_count);
    g_autoptr (GPtrArray) ranked = g_ptr_array_new ();
    g_ptr_array_extend (ranked, candidates, NULL, NULL);
    g_ptr_array_sort (ranked, by_length);
    for (guint i = 0; i < ranked->len; i++) {
      if (i < n)
        g_ptr_array_add (chosen, ranked->pdata[i]);
      else
        REJECT ((const BroTitle *) ranked->pdata[i], m1 (BRO_MSG_TITLES_REASON_NOT_LONGEST, "count", bro_json_value_new_integer (n)));
    }
    break;
  }
  case BRO_TITLE_STRATEGY_INDICES: {
    IndexPattern p;
    if (!index_pattern_parse (rules->index_pattern, &p)) {
      index_pattern_clear (&p);
      bro_bro_message_free (sel->error);
      sel->error = bro_bro_message_new (BRO_MSG_CONFIG_INVALID_INDEX_PATTERN, NULL, BRO_SEVERITY_ERROR);
      for (guint i = 0; i < candidates->len; i++)
        REJECT ((const BroTitle *) candidates->pdata[i], m0 (BRO_MSG_TITLES_REASON_INVALID_PATTERN));
      break;
    }
    gboolean by_source = rules->index_base == BRO_INDEX_BASE_SOURCE;
    int max_value = 0;
    for (guint i = 0; i < titles->len; i++) {
      const BroTitle *t = titles->pdata[i];
      int v = by_source ? (t->has_source_title_id ? t->source_title_id : 0) : t->index;
      if (i == 0 || v > max_value)
        max_value = v;
    }
    for (guint i = 0; i < candidates->len; i++) {
      const BroTitle *t = candidates->pdata[i];
      int v = by_source ? (t->has_source_title_id ? t->source_title_id : -1) : t->index;
      if (index_pattern_matches (&p, v, max_value))
        g_ptr_array_add (chosen, (gpointer) t);
      else
        REJECT (t, m0 (BRO_MSG_TITLES_REASON_NOT_IN_PATTERN));
    }
    index_pattern_clear (&p);
    break;
  }
  case BRO_TITLE_STRATEGY_MANUAL:
    sel->requires_manual_choice = TRUE;
    for (guint i = 0; i < candidates->len; i++)
      SET_REASON ((const BroTitle *) candidates->pdata[i], FALSE, m0 (BRO_MSG_TITLES_REASON_CHOOSE_MANUALLY));
    break;
  }

  g_ptr_array_sort (chosen, by_index);
  if (rules->max_titles > 0 && chosen->len > (guint) rules->max_titles) {
    for (guint i = rules->max_titles; i < chosen->len; i++)
      REJECT ((const BroTitle *) chosen->pdata[i], m1 (BRO_MSG_TITLES_REASON_OVER_LIMIT, "count", bro_json_value_new_integer (rules->max_titles)));
    g_ptr_array_set_size (chosen, rules->max_titles);
  }
  for (guint i = 0; i < chosen->len; i++) {
    const BroTitle *t = chosen->pdata[i];
    SET_REASON (t, TRUE, m0 (BRO_MSG_TITLES_REASON_SELECTED));
    g_array_append_val (sel->indices, t->index);
  }
  for (guint i = 0; i < titles->len; i++) {
    const BroTitle *t = titles->pdata[i];
    Reason *r = g_hash_table_lookup (reasons, GINT_TO_POINTER (t->index));
    if (r && r->set)
      g_ptr_array_add (sel->trace, bro_selection_trace_new (t->index, r->selected, bro_bro_message_copy (r->reason)));
    else
      g_ptr_array_add (sel->trace, bro_selection_trace_new (t->index, FALSE, m0 (BRO_MSG_TITLES_REASON_EXCLUDED)));
  }
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init (&it, reasons);
  while (g_hash_table_iter_next (&it, NULL, &v))
    bro_bro_message_free (((Reason *) v)->reason);
  return sel;
#undef REJECT
#undef SET_REASON
}
