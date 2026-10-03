/* bro-planned-output.c */
#include "bro-planned-output.h"

BroPlannedOutput *
bro_planned_output_new (BroPathRole role)
{
  BroPlannedOutput *o = g_new0 (BroPlannedOutput, 1);
  o->role = role;
  o->values = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o->title = o->episode = -1;
  o->extension = g_strdup ("");
  return o;
}

void
bro_planned_output_free (BroPlannedOutput *o)
{
  if (!o)
    return;
  g_hash_table_unref (o->values);
  g_free (o->extension);
  g_free (o);
}
