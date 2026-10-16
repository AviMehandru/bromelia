/* bro-drive-dto.c */
#include "bro-drive-dto.h"

BroDriveDto *
bro_drive_dto_new (void)
{
  return g_new0 (BroDriveDto, 1);
}

BroDriveDto *
bro_drive_dto_copy (const BroDriveDto *d)
{
  BroDriveDto *c;
  if (!d)
    return NULL;
  c = g_new0 (BroDriveDto, 1);
  *c = *d;
  c->id = g_strdup (d->id);
  c->config_id = g_strdup (d->config_id);
  c->name = g_strdup (d->name);
  c->identification = g_strdup (d->identification);
  c->device = g_strdup (d->device);
  c->medium = bro_medium_dto_copy (d->medium);
  c->profile_id = g_strdup (d->profile_id);
  return c;
}

void
bro_drive_dto_free (BroDriveDto *d)
{
  if (!d)
    return;
  g_free (d->id);
  g_free (d->config_id);
  g_free (d->name);
  g_free (d->identification);
  g_free (d->device);
  bro_medium_dto_free (d->medium);
  g_free (d->profile_id);
  g_free (d);
}

BroJsonValue *
bro_drive_dto_json (const BroDriveDto *d)
{
  BroJsonValue *m = bro_json_value_new_object ();
  bro_json_value_set (m, "id", bro_json_value_new_string (d->id));
  if (d->config_id)
    bro_json_value_set (m, "configId", bro_json_value_new_string (d->config_id));
  bro_json_value_set (m, "name", bro_json_value_new_string (d->name));
  bro_json_value_set (m, "identification", bro_json_value_new_string (d->identification));
  bro_json_value_set (m, "device", bro_json_value_new_string (d->device));
  bro_json_value_set (m, "makemkvIndex", d->has_makemkv_index ? bro_json_value_new_integer (d->makemkv_index) : bro_json_value_new_null ());
  bro_json_value_set (m, "connected", bro_json_value_new_bool (d->connected));
  bro_json_value_set (m, "state", bro_json_value_new_string (bro_drive_status_to_wire (d->state)));
  if (d->medium)
    bro_json_value_set (m, "medium", bro_medium_dto_json (d->medium));
  bro_json_value_set (m, "busy", bro_json_value_new_bool (d->busy));
  if (d->has_job_id)
    bro_json_value_set (m, "jobId", bro_json_value_new_string (d->job_id.value));
  bro_json_value_set (m, "profileId", bro_json_value_new_string (d->profile_id));
  bro_json_value_set (m, "ripOnInsert", bro_json_value_new_bool (d->rip_on_insert));
  return m;
}
