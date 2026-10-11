/* bro-lease-snapshot.c */
#include "bro-lease-snapshot.h"

#include <string.h>

BroLeaseSnapshot *
bro_lease_snapshot_new (void)
{
  BroLeaseSnapshot *s = g_new0 (BroLeaseSnapshot, 1);
  s->drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_holder_free);
  return s;
}

void
bro_lease_snapshot_free (BroLeaseSnapshot *snapshot)
{
  if (!snapshot)
    return;
  g_ptr_array_unref (snapshot->drives);
  g_free (snapshot);
}

BroJsonValue *
bro_lease_snapshot_json (const BroLeaseSnapshot *snapshot)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *drives = bro_json_value_new_array ();
  BroJsonValue *jobs = bro_json_value_new_object ();
  for (guint i = 0; i < snapshot->drives->len; i++)
    {
      BroDriveHolder *h = g_ptr_array_index (snapshot->drives, i);
      BroJsonValue *d = bro_json_value_new_object ();
      bro_json_value_set (d, "drive", bro_json_value_new_string (h->drive));
      bro_json_value_set (d, "job", bro_json_value_new_string (h->job.value));
      bro_json_value_append (drives, d);
    }
  bro_json_value_set (jobs, "acquisition", bro_json_value_new_integer (snapshot->jobs.acquisition));
  bro_json_value_set (jobs, "processing", bro_json_value_new_integer (snapshot->jobs.processing));
  bro_json_value_set (jobs, "maintenance", bro_json_value_new_integer (snapshot->jobs.maintenance));
  bro_json_value_set (o, "drives", drives);
  bro_json_value_set (o, "acquisitions", bro_json_value_new_integer (snapshot->acquisitions));
  bro_json_value_set (o, "jobs", jobs);
  return o;
}

static gboolean
integer (const BroJsonValue *json, const char *key, int *out)
{
  BroJsonValue *v = json ? bro_json_value_member (json, key) : NULL;
  if (!v || v->kind != BRO_JSON_VALUE_INTEGER)
    return FALSE;
  *out = (int) v->integer;
  return TRUE;
}

BroLeaseSnapshot *
bro_lease_snapshot_parse (const BroJsonValue *json)
{
  g_autoptr (BroLeaseSnapshot) s = bro_lease_snapshot_new ();
  BroJsonValue *drives = json ? bro_json_value_member (json, "drives") : NULL;
  BroJsonValue *jobs = json ? bro_json_value_member (json, "jobs") : NULL;
  if (!drives || drives->kind != BRO_JSON_VALUE_ARRAY || !integer (json, "acquisitions", &s->acquisitions)
      || !integer (jobs, "acquisition", &s->jobs.acquisition) || !integer (jobs, "processing", &s->jobs.processing)
      || !integer (jobs, "maintenance", &s->jobs.maintenance))
    return NULL;
  for (guint i = 0; i < bro_json_value_length (drives); i++)
    {
      BroJsonValue *d = bro_json_value_at (drives, i);
      BroJsonValue *drive = bro_json_value_member (d, "drive"), *job = bro_json_value_member (d, "job");
      BroId id = { { 0 } };
      if (!drive || drive->kind != BRO_JSON_VALUE_STRING || !job || job->kind != BRO_JSON_VALUE_STRING
          || strlen (job->string) >= sizeof id.value)
        return NULL;
      g_strlcpy (id.value, job->string, sizeof id.value);
      g_ptr_array_add (s->drives, bro_drive_holder_new (drive->string, &id));
    }
  return g_steal_pointer (&s);
}
