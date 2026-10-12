/* bro-step-sources.c */
#include "bro-step-sources.h"
#include "bro-message-code.h"
#include "bro-profile-resolver.h"
#include "bro-source-resolver.h"

#include <string.h>

static void
not_found (BroBroError **error, const char *drive_id)
{
  BroJsonValue *p = bro_json_value_new_object ();
  bro_json_value_set (p, "driveId", bro_json_value_new_string (drive_id));
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_DRIVE_NOT_FOUND), p);
}

BroDiscRequest *
bro_step_sources_request (const BroJobSnapshot *job, BroBroError **error)
{
  BroDiscRequest *r = bro_disc_request_decode (job->request);
  if (!r) {
    BroJsonValue *p = bro_json_value_new_object ();
    bro_json_value_set (p, "detail", bro_json_value_new_string ("the job has no source"));
    bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_INTERNAL_UNEXPECTED), p);
  }
  return r;
}

BroDriveLocation *
bro_step_sources_drive (BroDriveLookup *lookup, const char *drive_id, BroBroError **error)
{
  BroDriveLocation *at = lookup ? bro_drive_lookup_find (lookup, drive_id) : NULL;
  if (!at)
    not_found (error, drive_id);
  return at;
}

BroMakemkvSource *
bro_step_sources_makemkv (const BroDiscSource *source, BroDriveLookup *lookup, BroBroError **error)
{
  switch (source->kind) {
  case BRO_DISC_SOURCE_DRIVE: {
    g_autoptr (BroDriveLocation) at = bro_step_sources_drive (lookup, source->value, error);
    if (!at)
      return NULL;
    if (!at->has_makemkv_index) {
      not_found (error, source->value);
      return NULL;
    }
    return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_DRIVE, at->makemkv_index, at->device, NULL);
  }
  case BRO_DISC_SOURCE_ISO:
    return bro_makemkv_source_new (BRO_MAKEMKV_SOURCE_ISO, 0, NULL, source->value);
  case BRO_DISC_SOURCE_FOLDER:
    return bro_source_resolver_source (source->value, TRUE);
  case BRO_DISC_SOURCE_FILE:
    return bro_source_resolver_source (source->value, FALSE);
  }
  bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_INTERNAL_UNEXPECTED), NULL);
  return NULL;
}

BroEffectiveProfile *
bro_step_sources_profile (const BroStepContext *context, const char *config_id)
{
  BroRuleFacts facts = { "", "", "", NULL, NULL, NULL, context->job->request->automatic };
  return bro_profile_resolver_resolve (context->config, config_id, &facts, NULL);
}

BroRipMode
bro_step_sources_base_mode (const BroJobSnapshot *job, const BroEffectiveProfile *profile)
{
  BroRipMode mode;
  if (bro_rip_mode_from_wire (bro_json_value_get_string (bro_json_value_member (job->request->options, "mode"), ""), &mode))
    return mode;
  if (bro_rip_mode_from_wire (
          bro_json_value_get_string (bro_json_value_member (bro_json_value_member (profile->profile, "mode"), "default"), ""), &mode))
    return mode;
  return BRO_RIP_MODE_MKV;
}

/* The last part of a path, ignoring separators at its end. */
static char *
last_part (const char *path)
{
  g_autofree char *p = g_strdup (path);
  gsize n = strlen (p);
  while (n > 0 && (p[n - 1] == '/' || p[n - 1] == '\\'))
    p[--n] = '\0';
  const char *slash = NULL;
  for (const char *c = p; *c; c++)
    if (*c == '/' || *c == '\\')
      slash = c;
  return g_strdup (slash ? slash + 1 : p);
}

char *
bro_step_sources_label (const BroDiscSource *source, const BroDriveLocation *drive)
{
  switch (source->kind) {
  case BRO_DISC_SOURCE_DRIVE:
    return g_strdup (drive ? drive->label : "");
  case BRO_DISC_SOURCE_ISO: {
    char *name = last_part (source->value);
    char *dot = strrchr (name, '.');
    if (dot && dot > name)
      *dot = '\0';
    return name;
  }
  case BRO_DISC_SOURCE_FOLDER:
    return last_part (source->value);
  case BRO_DISC_SOURCE_FILE: {
    g_autoptr (BroMakemkvSource) root = bro_source_resolver_source (source->value, FALSE);
    return last_part (root->kind == BRO_MAKEMKV_SOURCE_FILE ? root->path : source->value);
  }
  }
  return g_strdup ("");
}
