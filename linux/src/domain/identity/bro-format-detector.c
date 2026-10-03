/* bro-format-detector.c */
#include "bro-format-detector.h"
#include "bro-attribute-id.h"

#include <string.h>

static gboolean
is_uhd (const BroListing *listing)
{
  for (guint i = 0; i < listing->titles->len; i++) {
    const BroTitle *t = listing->titles->pdata[i];
    for (guint k = 0; k < t->tracks->len; k++) {
      const BroTrack *tr = t->tracks->pdata[k];
      if (tr->kind != BRO_TRACK_KIND_VIDEO)
        continue;
      const char *size = g_hash_table_lookup (tr->attributes, GINT_TO_POINTER (BRO_ATTRIBUTE_ID_VIDEO_SIZE));
      const char *id = g_hash_table_lookup (tr->attributes, GINT_TO_POINTER (BRO_ATTRIBUTE_ID_CODEC_ID));
      const char *shrt = g_hash_table_lookup (tr->attributes, GINT_TO_POINTER (BRO_ATTRIBUTE_ID_CODEC_SHORT));
      g_autofree char *codec_raw = g_strconcat (id ? id : "", " ", shrt ? shrt : "", NULL);
      g_autofree char *codec = g_ascii_strdown (codec_raw, -1);
      if ((size && (strstr (size, "2160") || strstr (size, "3840"))) || strstr (codec, "hevc") || strstr (codec, "mpegh"))
        return TRUE;
    }
  }
  return FALSE;
}

BroDiscFormat
bro_format_detector_detect (const BroListing *listing, const BroDiscFlags *flags, const char *index_bdmv, gboolean has_video_ts)
{
  if (listing) {
    g_autofree char *t = g_ascii_strdown (listing->type_text, -1);
    if (strstr (t, "blu"))
      return is_uhd (listing) ? BRO_DISC_FORMAT_UHD : BRO_DISC_FORMAT_BLURAY;
    if (strstr (t, "hd"))
      return BRO_DISC_FORMAT_HDDVD;
    if (strstr (t, "dvd"))
      return BRO_DISC_FORMAT_DVD;
    if (is_uhd (listing))
      return BRO_DISC_FORMAT_UHD;
  }
  if (has_video_ts)
    return BRO_DISC_FORMAT_DVD;
  if (index_bdmv && g_str_has_prefix (index_bdmv, "INDX"))
    return g_str_has_prefix (index_bdmv, "INDX0300") ? BRO_DISC_FORMAT_UHD : BRO_DISC_FORMAT_BLURAY;
  if (flags) {
    if (flags->raw & BRO_DISC_FLAGS_BLURAY_FILES)
      return BRO_DISC_FORMAT_BLURAY;
    if (flags->raw & BRO_DISC_FLAGS_HD_DVD_FILES)
      return BRO_DISC_FORMAT_HDDVD;
    if (flags->raw & BRO_DISC_FLAGS_DVD_FILES)
      return BRO_DISC_FORMAT_DVD;
  }
  return BRO_DISC_FORMAT_UNKNOWN;
}

BroFormatCode
bro_format_detector_code (BroDiscFormat format, gboolean encrypted)
{
  const char *b;
  switch (format) {
  case BRO_DISC_FORMAT_DVD: b = "DVD"; break;
  case BRO_DISC_FORMAT_BLURAY: b = "BR"; break;
  case BRO_DISC_FORMAT_UHD: b = "4K"; break;
  case BRO_DISC_FORMAT_HDDVD: b = "HDDVD"; break;
  default: b = "DISC"; break;
  }
  BroFormatCode code;
  g_snprintf (code.text, sizeof code.text, "%s%s", b, encrypted ? "e" : "");
  return code;
}
