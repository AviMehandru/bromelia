/* bro-mode-chooser.c */
#include "bro-mode-chooser.h"

#include "bro-config-private.h"
#include "bro-issue.h"

#define M bro_json_value_member

gboolean
bro_mode_chooser_mode (BroDiscFormat format, BroDiscFlags flags, BroDiscContent content, const BroProfile *profile,
                       gboolean chosen_by_hand, BroRipMode *out)
{
  g_autoptr (GPtrArray) issues = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_issue_free);
  g_autoptr (BroJsonValue) p = _bro_schema_normalize_full (profile->json, _bro_schema_def ("ProfileFields"), "", TRUE, TRUE, issues);
  if (!(flags.raw & (BRO_DISC_FLAGS_DVD_FILES | BRO_DISC_FLAGS_BLURAY_FILES | BRO_DISC_FLAGS_HD_DVD_FILES))) {
    BroJsonValue *other = M (p, "otherDiscs");
    switch (content) {
    case BRO_DISC_CONTENT_AUDIO:
      *out = BRO_RIP_MODE_AUDIO_CD;
      return bro_json_value_get_bool (M (other, "ripAudioCDs"), FALSE);
    case BRO_DISC_CONTENT_DATA:
      *out = BRO_RIP_MODE_DATA_IMAGE;
      return bro_json_value_get_bool (M (other, "imageDataDiscs"), FALSE);
    case BRO_DISC_CONTENT_BLANK:
      return FALSE;
    case BRO_DISC_CONTENT_VIDEO:
    case BRO_DISC_CONTENT_UNKNOWN:
      break;
    }
  }
  BroJsonValue *mode = M (p, "mode");
  const char *by_format = NULL;
  if (!chosen_by_hand && format != BRO_DISC_FORMAT_UNKNOWN)
    by_format = bro_json_value_get_string (M (M (mode, "byFormat"), bro_disc_format_to_wire (format)), NULL);
  if (!bro_rip_mode_from_wire (by_format ? by_format : bro_json_value_get_string (M (mode, "default"), ""), out))
    *out = BRO_RIP_MODE_MKV;
  return TRUE;
}
