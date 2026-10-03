/* bro-rip-check.c */
#include "bro-rip-check.h"

#include <math.h>
#include <string.h>

static BroBroMessage *
counts (BroMessageCode code, int have, int listed)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "have", bro_json_value_new_integer (have));
  bro_json_value_set (params, "listed", bro_json_value_new_integer (listed));
  return bro_bro_message_new (code, params, BRO_SEVERITY_INFO);
}

BroRipCheckResult *
bro_rip_check_check (const BroMkvProbe *probe, const BroTitle *title)
{
  BroRipCheckResult *r = bro_rip_check_result_new ();
  guint n = probe->tracks->len;
  gboolean has_video = FALSE, title_video = FALSE;
  for (guint i = 0; i < n; i++)
    has_video |= strcmp (((BroMkvTrack *) probe->tracks->pdata[i])->type, "video") == 0;
  for (guint i = 0; title->tracks && i < title->tracks->len; i++)
    title_video |= ((BroTrack *) title->tracks->pdata[i])->kind == BRO_TRACK_KIND_VIDEO;
  guint listed = title->tracks ? title->tracks->len : 0;
  if (n == 0)
    g_ptr_array_add (r->problems, bro_bro_message_new (BRO_MSG_RIPCHECK_NO_TRACKS, NULL, BRO_SEVERITY_INFO));
  if (n > 0 && !has_video && title_video)
    g_ptr_array_add (r->problems, bro_bro_message_new (BRO_MSG_RIPCHECK_NO_VIDEO, NULL, BRO_SEVERITY_INFO));
  double expected = title->duration_seconds;
  if (expected > 0) {
    if (probe->has_duration) {
      if (fabs (probe->duration_seconds - expected) > bro_rip_check_tolerance (expected).seconds) {
        BroJsonValue *params = bro_json_value_new_object ();
        char *actual = bro_duration_format_clock ((BroDuration) { round (probe->duration_seconds) });
        char *want = bro_duration_format_clock ((BroDuration) { expected });
        bro_json_value_set (params, "actual", bro_json_value_new_string (actual));
        bro_json_value_set (params, "expected", bro_json_value_new_string (want));
        g_free (actual);
        g_free (want);
        g_ptr_array_add (r->problems, bro_bro_message_new (BRO_MSG_RIPCHECK_DURATION, params, BRO_SEVERITY_INFO));
      }
    } else {
      g_ptr_array_add (r->problems, bro_bro_message_new (BRO_MSG_RIPCHECK_NO_DURATION, NULL, BRO_SEVERITY_INFO));
    }
  }
  if (listed > 0 && n > listed)
    g_ptr_array_add (r->notes, counts (BRO_MSG_RIPCHECK_MORE_TRACKS, (int) n, (int) listed));
  if (title->chapters > 1 && (probe->chapter_count < title->chapters || probe->chapter_count > title->chapters + 1))
    g_ptr_array_add (r->notes, counts (BRO_MSG_RIPCHECK_CHAPTERS, probe->chapter_count, title->chapters));
  return r;
}

BroDuration
bro_rip_check_tolerance (double expected_seconds)
{
  return (BroDuration) { MAX (5.0, expected_seconds * 0.005) };
}
