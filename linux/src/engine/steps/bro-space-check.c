/* bro-space-check.c */
#include "bro-space-check.h"
#include "bro-space-estimate.h"
#include "bro-steps-private.h"
#include "bro-volume-info.h"

gboolean
bro_space_check_reserve (const BroStepContext *context, const char *library_root, gint64 need, BroBroError **error)
{
  g_autoptr (BroVolumeInfo) volume = bro_file_system_volume (context->fs, library_root, error);
  BroBytes bytes = { need };
  BroJsonValue *params;
  if (!volume)
    return FALSE;
  params = _bro_param ("free", bro_json_value_new_integer (volume->free_bytes));
  bro_json_value_set (params, "need", bro_json_value_new_integer (need));
  _bro_step_log (context, BRO_MSG_SPACE_REPORT, params, BRO_SEVERITY_INFO);
  return bro_space_reservations_reserve (context->space, volume->id, library_root, bro_space_estimate_required (bytes).count, volume->free_bytes,
                                         error);
}
