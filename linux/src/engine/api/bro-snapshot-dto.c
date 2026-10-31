/* bro-snapshot-dto.c */
#include "bro-snapshot-dto.h"
#include "bro-drive-dto.h"
#include "bro-job-dto.h"
#include "bro-library-dto.h"
#include "bro-problem-dto.h"

BroSnapshotDto *
bro_snapshot_dto_new (void)
{
  BroSnapshotDto *s = g_new0 (BroSnapshotDto, 1);
  s->drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_dto_free);
  s->jobs = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_job_dto_free);
  s->queues = bro_queue_orders_new ();
  s->problems = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_problem_dto_free);
  s->libraries = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_library_dto_free);
  return s;
}

void
bro_snapshot_dto_free (BroSnapshotDto *snapshot)
{
  if (!snapshot)
    return;
  bro_system_status_dto_free (snapshot->system);
  g_ptr_array_unref (snapshot->drives);
  g_ptr_array_unref (snapshot->jobs);
  bro_queue_orders_free (snapshot->queues);
  g_ptr_array_unref (snapshot->problems);
  g_ptr_array_unref (snapshot->libraries);
  g_free (snapshot);
}

static BroJsonValue *
ids (const GArray *ids)
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (guint i = 0; i < ids->len; i++)
    bro_json_value_append (a, bro_json_value_new_string (g_array_index (ids, BroId, i).value));
  return a;
}

BroJsonValue *
bro_snapshot_dto_json (const BroSnapshotDto *snapshot)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *drives = bro_json_value_new_array ();
  BroJsonValue *jobs = bro_json_value_new_array ();
  BroJsonValue *queues = bro_json_value_new_object ();
  BroJsonValue *problems = bro_json_value_new_array ();
  BroJsonValue *libraries = bro_json_value_new_array ();
  bro_json_value_set (o, "system", bro_system_status_dto_json (snapshot->system));
  for (guint i = 0; i < snapshot->drives->len; i++)
    bro_json_value_append (drives, bro_drive_dto_json (g_ptr_array_index (snapshot->drives, i)));
  bro_json_value_set (o, "drives", drives);
  bro_json_value_set (o, "sessions", bro_json_value_new_array ());
  for (guint i = 0; i < snapshot->jobs->len; i++)
    bro_json_value_append (jobs, bro_job_dto_json (g_ptr_array_index (snapshot->jobs, i)));
  bro_json_value_set (o, "jobs", jobs);
  bro_json_value_set (queues, "acquisition", ids (snapshot->queues->acquisition));
  bro_json_value_set (queues, "processing", ids (snapshot->queues->processing));
  bro_json_value_set (queues, "maintenance", ids (snapshot->queues->maintenance));
  bro_json_value_set (o, "queues", queues);
  for (guint i = 0; i < snapshot->problems->len; i++)
    bro_json_value_append (problems, bro_problem_dto_json (g_ptr_array_index (snapshot->problems, i)));
  bro_json_value_set (o, "problems", problems);
  for (guint i = 0; i < snapshot->libraries->len; i++)
    bro_json_value_append (libraries, bro_library_dto_json (g_ptr_array_index (snapshot->libraries, i)));
  bro_json_value_set (o, "libraries", libraries);
  bro_json_value_set (o, "configVersion", bro_json_value_new_integer (snapshot->config_version));
  return o;
}
