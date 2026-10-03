/* bro-remux.c */
#include "bro-remux.h"

#include "bro-mkv-track.h"

#include <string.h>

static const char *
expected_type (BroTrackKind kind)
{
  switch (kind) {
  case BRO_TRACK_KIND_VIDEO:
    return "video";
  case BRO_TRACK_KIND_AUDIO:
    return "audio";
  case BRO_TRACK_KIND_SUBTITLE:
    return "subtitles";
  default:
    return NULL;
  }
}

static gboolean
kept (GArray *keep, int index)
{
  for (guint i = 0; i < keep->len; i++)
    if (g_array_index (keep, int, i) == index)
      return TRUE;
  return FALSE;
}

/* A type the file has: the tracks kept, or none of them. */
static void
add (GPtrArray *args, GPtrArray *layout, const BroTitle *title, GArray *keep, const char *type, const char *tracks, const char *none)
{
  g_autoptr (GString) ids = g_string_new (NULL);
  gboolean present = FALSE;
  for (guint i = 0; i < layout->len; i++) {
    BroMkvTrack *t = layout->pdata[i];
    if (strcmp (t->type, type) != 0)
      continue;
    present = TRUE;
    if (kept (keep, ((BroTrack *) title->tracks->pdata[i])->index))
      g_string_append_printf (ids, "%s%d", ids->len ? "," : "", t->id);
  }
  if (ids->len) {
    g_ptr_array_add (args, g_strdup (tracks));
    g_ptr_array_add (args, g_strdup (ids->str));
  } else if (present) {
    g_ptr_array_add (args, g_strdup (none));
  }
}

GStrv
bro_remux_arguments (GPtrArray *layout, const BroTitle *title, GArray *keep, const char *input, const char *output)
{
  if (!title->tracks || layout->len != title->tracks->len)
    return NULL;
  for (guint i = 0; i < layout->len; i++) {
    const char *type = expected_type (((BroTrack *) title->tracks->pdata[i])->kind);
    if (type && strcmp (type, ((BroMkvTrack *) layout->pdata[i])->type) != 0)
      return NULL;
  }
  GPtrArray *args = g_ptr_array_new ();
  g_ptr_array_add (args, g_strdup ("-o"));
  g_ptr_array_add (args, g_strdup (output));
  add (args, layout, title, keep, "video", "--video-tracks", "--no-video");
  add (args, layout, title, keep, "audio", "--audio-tracks", "--no-audio");
  add (args, layout, title, keep, "subtitles", "--subtitle-tracks", "--no-subtitles");
  g_ptr_array_add (args, g_strdup (input));
  g_ptr_array_add (args, NULL);
  return (GStrv) g_ptr_array_free (args, FALSE);
}
