/* bro-plan-output.c */
#include "bro-plan-output.h"

BroPlanOutput *
bro_plan_output_new (void)
{
  BroPlanOutput *p = g_new0 (BroPlanOutput, 1);
  g_autoptr (BroJsonValue) empty = bro_json_value_new_object ();
  p->mode = BRO_RIP_MODE_MKV;
  p->profile_id = g_strdup ("");
  p->profile = bro_json_value_new_object ();
  p->library_id = g_strdup ("");
  p->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_title_choice_free);
  p->identity = bro_identity_decode (empty);
  return p;
}

void
bro_plan_output_free (BroPlanOutput *plan)
{
  if (!plan)
    return;
  g_free (plan->profile_id);
  bro_json_value_unref (plan->profile);
  g_free (plan->library_id);
  g_ptr_array_unref (plan->titles);
  bro_archived_match_free (plan->ask_archived);
  bro_identity_free (plan->identity);
  g_free (plan);
}

BroJsonValue *
bro_plan_output_encode (const BroPlanOutput *plan)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *titles = bro_json_value_new_array ();
  bro_json_value_set (o, "mode", bro_json_value_new_string (bro_rip_mode_to_wire (plan->mode)));
  bro_json_value_set (o, "profileId", bro_json_value_new_string (plan->profile_id));
  bro_json_value_set (o, "profile", bro_json_value_ref (plan->profile));
  bro_json_value_set (o, "libraryId", bro_json_value_new_string (plan->library_id));
  bro_json_value_set (o, "libraryOffline", bro_json_value_new_bool (plan->library_offline));
  for (guint i = 0; i < plan->titles->len; i++)
    {
      const BroTitleChoice *t = g_ptr_array_index (plan->titles, i);
      BroJsonValue *title = bro_json_value_new_object ();
      bro_json_value_set (title, "index", bro_json_value_new_integer (t->index));
      if (t->tracks)
        {
          BroJsonValue *tracks = bro_json_value_new_array ();
          for (guint k = 0; k < t->tracks->len; k++)
            bro_json_value_append (tracks, bro_json_value_new_integer (g_array_index (t->tracks, int, k)));
          bro_json_value_set (title, "tracks", tracks);
        }
      if (t->name && *t->name)
        bro_json_value_set (title, "name", bro_json_value_new_string (t->name));
      bro_json_value_append (titles, title);
    }
  bro_json_value_set (o, "titles", titles);
  bro_json_value_set (o, "onePass", plan->has_one_pass_min_length ? bro_json_value_new_integer (plan->one_pass_min_length) : bro_json_value_new_null ());
  bro_json_value_set (o, "spaceNeed", plan->has_space_need ? bro_json_value_new_integer (plan->space_need) : bro_json_value_new_null ());
  bro_json_value_set (o, "spaceReserved", bro_json_value_new_bool (plan->space_reserved));
  bro_json_value_set (o, "askArchived", plan->ask_archived ? bro_archived_match_encode (plan->ask_archived) : bro_json_value_new_null ());
  bro_json_value_set (o, "identity", bro_identity_encode (plan->identity));
  return o;
}

BroPlanOutput *
bro_plan_output_decode (const BroJsonValue *json)
{
  BroPlanOutput *p = bro_plan_output_new ();
  BroJsonValue *v;
  BroJsonValue *titles = bro_json_value_member (json, "titles");
  if (!bro_rip_mode_from_wire (bro_json_value_get_string (bro_json_value_member (json, "mode"), ""), &p->mode))
    p->mode = BRO_RIP_MODE_MKV;
  g_free (p->profile_id);
  p->profile_id = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "profileId"), ""));
  if ((v = bro_json_value_member (json, "profile")) && v->kind == BRO_JSON_VALUE_OBJECT)
    {
      bro_json_value_unref (p->profile);
      p->profile = bro_json_value_ref (v);
    }
  g_free (p->library_id);
  p->library_id = g_strdup (bro_json_value_get_string (bro_json_value_member (json, "libraryId"), ""));
  p->library_offline = bro_json_value_get_bool (bro_json_value_member (json, "libraryOffline"), FALSE);
  for (guint i = 0; i < bro_json_value_length (titles); i++)
    {
      BroJsonValue *t = bro_json_value_at (titles, i);
      BroJsonValue *tracks = bro_json_value_member (t, "tracks");
      GArray *picked = NULL;
      if (tracks && tracks->kind == BRO_JSON_VALUE_ARRAY)
        {
          picked = g_array_new (FALSE, FALSE, sizeof (int));
          for (guint k = 0; k < bro_json_value_length (tracks); k++)
            {
              int track = (int) bro_json_value_get_integer (bro_json_value_at (tracks, k), 0);
              g_array_append_val (picked, track);
            }
        }
      g_ptr_array_add (p->titles, bro_title_choice_new ((int) bro_json_value_get_integer (bro_json_value_member (t, "index"), 0), TRUE, picked,
                                                        bro_json_value_get_string (bro_json_value_member (t, "name"), "")));
    }
  if ((v = bro_json_value_member (json, "onePass")) && v->kind == BRO_JSON_VALUE_INTEGER)
    {
      p->has_one_pass_min_length = TRUE;
      p->one_pass_min_length = (int) v->integer;
    }
  if ((v = bro_json_value_member (json, "spaceNeed")) && v->kind == BRO_JSON_VALUE_INTEGER)
    {
      p->has_space_need = TRUE;
      p->space_need = v->integer;
    }
  p->space_reserved = bro_json_value_get_bool (bro_json_value_member (json, "spaceReserved"), FALSE);
  p->ask_archived = bro_archived_match_decode (bro_json_value_member (json, "askArchived"));
  if ((v = bro_json_value_member (json, "identity")) && v->kind == BRO_JSON_VALUE_OBJECT)
    {
      bro_identity_free (p->identity);
      p->identity = bro_identity_decode (v);
    }
  return p;
}

BroPlanOutput *
bro_plan_output_of (const BroJobSnapshot *job)
{
  BroJsonValue *o = bro_job_snapshot_output (job, BRO_STEP_KIND_PLAN);
  return o && o->kind == BRO_JSON_VALUE_OBJECT ? bro_plan_output_decode (o) : NULL;
}
