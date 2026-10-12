/* bro-session-choices.c */
#include "bro-session-choices.h"

BroTitleChoice *
bro_title_choice_new (int index, gboolean selected, GArray *tracks, const char *name)
{
  BroTitleChoice *c = g_new0 (BroTitleChoice, 1);
  c->index = index;
  c->selected = selected;
  c->tracks = tracks;
  c->name = g_strdup (name ? name : "");
  return c;
}

BroTitleChoice *
bro_title_choice_copy (const BroTitleChoice *choice)
{
  GArray *tracks = NULL;
  if (choice->tracks) {
    tracks = g_array_sized_new (FALSE, FALSE, sizeof (int), choice->tracks->len);
    g_array_append_vals (tracks, choice->tracks->data, choice->tracks->len);
  }
  return bro_title_choice_new (choice->index, choice->selected, tracks, choice->name);
}

void
bro_title_choice_free (BroTitleChoice *choice)
{
  if (!choice)
    return;
  if (choice->tracks)
    g_array_unref (choice->tracks);
  g_free (choice->name);
  g_free (choice);
}

BroSessionChoices *
bro_session_choices_new (void)
{
  BroSessionChoices *c = g_new0 (BroSessionChoices, 1);
  c->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_title_choice_free);
  c->name = g_strdup ("");
  c->online_id = g_strdup ("");
  return c;
}

BroSessionChoices *
bro_session_choices_copy (const BroSessionChoices *choices)
{
  BroSessionChoices *c = bro_session_choices_new ();
  for (guint i = 0; i < choices->titles->len; i++)
    g_ptr_array_add (c->titles, bro_title_choice_copy (choices->titles->pdata[i]));
  g_free (c->name);
  c->name = g_strdup (choices->name);
  c->has_kind = choices->has_kind;
  c->kind = choices->kind;
  c->has_year = choices->has_year;
  c->year = choices->year;
  g_free (c->online_id);
  c->online_id = g_strdup (choices->online_id);
  c->has_first_episode = choices->has_first_episode;
  c->first_episode = choices->first_episode;
  c->library = g_strdup (choices->library);
  c->profile = g_strdup (choices->profile);
  return c;
}

void
bro_session_choices_free (BroSessionChoices *choices)
{
  if (!choices)
    return;
  g_ptr_array_unref (choices->titles);
  g_free (choices->name);
  g_free (choices->online_id);
  g_free (choices->library);
  g_free (choices->profile);
  g_free (choices);
}

static BroJsonValue *
int_or_null (gboolean has, int v)
{
  return has ? bro_json_value_new_integer (v) : bro_json_value_new_null ();
}

static BroJsonValue *
string_or_null (const char *s)
{
  return s ? bro_json_value_new_string (s) : bro_json_value_new_null ();
}

BroJsonValue *
bro_session_choices_encode (const BroSessionChoices *c)
{
  BroJsonValue *titles = bro_json_value_new_array ();
  for (guint i = 0; i < c->titles->len; i++) {
    const BroTitleChoice *t = c->titles->pdata[i];
    BroJsonValue *o = bro_json_value_new_object ();
    bro_json_value_set (o, "index", bro_json_value_new_integer (t->index));
    bro_json_value_set (o, "selected", bro_json_value_new_bool (t->selected));
    if (t->tracks) {
      BroJsonValue *tracks = bro_json_value_new_array ();
      for (guint k = 0; k < t->tracks->len; k++)
        bro_json_value_append (tracks, bro_json_value_new_integer (g_array_index (t->tracks, int, k)));
      bro_json_value_set (o, "tracks", tracks);
    }
    if (*t->name)
      bro_json_value_set (o, "name", bro_json_value_new_string (t->name));
    bro_json_value_append (titles, o);
  }
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "titles", titles);
  bro_json_value_set (o, "name", bro_json_value_new_string (c->name));
  bro_json_value_set (o, "kind", c->has_kind ? bro_json_value_new_string (bro_media_kind_to_wire (c->kind)) : bro_json_value_new_null ());
  bro_json_value_set (o, "year", int_or_null (c->has_year, c->year));
  bro_json_value_set (o, "onlineId", bro_json_value_new_string (c->online_id));
  bro_json_value_set (o, "firstEpisode", int_or_null (c->has_first_episode, c->first_episode));
  bro_json_value_set (o, "library", string_or_null (c->library));
  bro_json_value_set (o, "profile", string_or_null (c->profile));
  return o;
}

/* TRUE and @out when the member is a number. */
static gboolean
optional_int (const BroJsonValue *json, const char *key, int *out)
{
  BroJsonValue *v = bro_json_value_member (json, key);
  if (!v || (v->kind != BRO_JSON_VALUE_INTEGER && v->kind != BRO_JSON_VALUE_NUMBER))
    return FALSE;
  *out = (int) bro_json_value_get_integer (v, 0);
  return TRUE;
}

static char *
optional_string (const BroJsonValue *json, const char *key)
{
  BroJsonValue *v = bro_json_value_member (json, key);
  return v && v->kind == BRO_JSON_VALUE_STRING ? g_strdup (v->string) : NULL;
}

BroSessionChoices *
bro_session_choices_decode (const BroJsonValue *json)
{
  BroSessionChoices *c = bro_session_choices_new ();
  BroJsonValue *titles = bro_json_value_member (json, "titles");
  for (guint i = 0; titles && titles->kind == BRO_JSON_VALUE_ARRAY && i < titles->items->len; i++) {
    const BroJsonValue *t = titles->items->pdata[i];
    BroJsonValue *tj = bro_json_value_member (t, "tracks");
    GArray *tracks = NULL;
    if (tj && tj->kind == BRO_JSON_VALUE_ARRAY) {
      tracks = g_array_new (FALSE, FALSE, sizeof (int));
      for (guint k = 0; k < tj->items->len; k++) {
        int v = (int) bro_json_value_get_integer (tj->items->pdata[k], 0);
        g_array_append_val (tracks, v);
      }
    }
    g_ptr_array_add (c->titles, bro_title_choice_new ((int) bro_json_value_get_integer (bro_json_value_member (t, "index"), 0),
                                                      bro_json_value_get_bool (bro_json_value_member (t, "selected"), FALSE), tracks,
                                                      bro_json_value_get_string (bro_json_value_member (t, "name"), "")));
  }
  g_free (c->name);
  c->name = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "name"), ""));
  c->has_kind = bro_media_kind_from_wire (bro_json_value_get_string (bro_json_value_member (json, "kind"), ""), &c->kind);
  c->has_year = optional_int (json, "year", &c->year);
  g_free (c->online_id);
  c->online_id = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "onlineId"), ""));
  c->has_first_episode = optional_int (json, "firstEpisode", &c->first_episode);
  c->library = optional_string (json, "library");
  c->profile = optional_string (json, "profile");
  return c;
}
