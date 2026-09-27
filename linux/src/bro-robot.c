/* bro-robot.c — makemkvcon robot protocol parser and disc model. */
#include "bro-robot.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

GPtrArray *
bro_split_fields (const char *s)
{
  GPtrArray *fields = g_ptr_array_new_with_free_func (g_free);
  GString *cur = g_string_new (NULL);
  gboolean in_quotes = FALSE, was_quoted = FALSE;

  for (const char *p = s; *p; p++)
    {
      char c = *p;
      if (in_quotes)
        {
          if (c == '\\' && p[1] != '\0')
            {
              if (p[1] == '"' || p[1] == '\\')
                {
                  g_string_append_c (cur, p[1]);
                  p++;
                }
              else
                g_string_append_c (cur, c);
            }
          else if (c == '"')
            in_quotes = FALSE;
          else
            g_string_append_c (cur, c);
        }
      else if (c == ',')
        {
          g_ptr_array_add (fields, g_string_free (cur, FALSE));
          cur = g_string_new (NULL);
          was_quoted = FALSE;
        }
      else if (c == '"' && cur->len == 0 && !was_quoted)
        {
          in_quotes = TRUE;
          was_quoted = TRUE;
        }
      else
        g_string_append_c (cur, c);
    }
  g_ptr_array_add (fields, g_string_free (cur, FALSE));
  return fields;
}

static gboolean
parse_int (const char *s, int *out)
{
  char *end = NULL;
  long v;
  if (s == NULL || *s == '\0')
    return FALSE;
  v = strtol (s, &end, 10);
  while (end && (*end == ' ' || *end == '\t'))
    end++;
  if (end == NULL || *end != '\0')
    return FALSE;
  *out = (int) v;
  return TRUE;
}

#define F(i) ((const char *) g_ptr_array_index (f, (i)))

BroEvent *
bro_event_parse (const char *raw)
{
  g_autofree char *line = g_strdup (raw ? raw : "");
  g_autoptr (GPtrArray) f = NULL;
  BroEvent *ev;
  char *colon, *body;
  size_t len = strlen (line);

  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
    line[--len] = '\0';
  if (len == 0)
    return NULL;

  ev = g_new0 (BroEvent, 1);
  colon = strchr (line, ':');
  if (colon == NULL)
    goto raw;
  *colon = '\0';
  body = colon + 1;

  if (g_str_equal (line, "MSG"))
    {
      f = bro_split_fields (body);
      if (f->len < 5 || !parse_int (F (0), &ev->code) || !parse_int (F (1), &ev->flags))
        goto restore;
      ev->type = BRO_EV_MESSAGE;
      ev->text = g_strdup (F (3));
      ev->format = g_strdup (F (4));
      ev->params = g_ptr_array_new_with_free_func (g_free);
      for (guint i = 5; i < f->len; i++)
        g_ptr_array_add (ev->params, g_strdup (F (i)));
      return ev;
    }
  if (g_str_equal (line, "PRGC") || g_str_equal (line, "PRGT"))
    {
      f = bro_split_fields (body);
      if (f->len < 3 || !parse_int (F (0), &ev->code) || !parse_int (F (1), &ev->id))
        goto restore;
      ev->type = line[3] == 'C' ? BRO_EV_PROGRESS_CURRENT : BRO_EV_PROGRESS_TOTAL;
      ev->text = g_strdup (F (2));
      return ev;
    }
  if (g_str_equal (line, "PRGV"))
    {
      f = bro_split_fields (body);
      if (f->len < 3 || !parse_int (F (0), &ev->current) || !parse_int (F (1), &ev->total) || !parse_int (F (2), &ev->max))
        goto restore;
      ev->type = BRO_EV_PROGRESS_VALUE;
      return ev;
    }
  if (g_str_equal (line, "DRV"))
    {
      int state;
      f = bro_split_fields (body);
      if (f->len < 7 || !parse_int (F (0), &ev->drive.index) || !parse_int (F (1), &state) || !parse_int (F (3), &ev->drive.flags))
        goto restore;
      ev->type = BRO_EV_DRIVE;
      ev->drive.state = (state == 0 || state == 1 || state == 2 || state == 3 || state == 257) ? (BroDriveState) state : BRO_DRIVE_NO_DRIVE;
      ev->drive.drive_name = g_strdup (F (4));
      ev->drive.disc_name = g_strdup (F (5));
      ev->drive.device = g_strdup (F (6));
      return ev;
    }
  if (g_str_equal (line, "TCOUNT"))
    {
      if (!parse_int (body, &ev->total))
        goto restore;
      ev->type = BRO_EV_TITLE_COUNT;
      return ev;
    }
  if (g_str_equal (line, "CINFO"))
    {
      f = bro_split_fields (body);
      if (f->len < 3 || !parse_int (F (0), &ev->id) || !parse_int (F (1), &ev->code))
        goto restore;
      ev->type = BRO_EV_CINFO;
      ev->text = g_strdup (F (2));
      return ev;
    }
  if (g_str_equal (line, "TINFO"))
    {
      f = bro_split_fields (body);
      if (f->len < 4 || !parse_int (F (0), &ev->title) || !parse_int (F (1), &ev->id) || !parse_int (F (2), &ev->code))
        goto restore;
      ev->type = BRO_EV_TINFO;
      ev->text = g_strdup (F (3));
      return ev;
    }
  if (g_str_equal (line, "SINFO"))
    {
      f = bro_split_fields (body);
      if (f->len < 5 || !parse_int (F (0), &ev->title) || !parse_int (F (1), &ev->stream) ||
          !parse_int (F (2), &ev->id) || !parse_int (F (3), &ev->code))
        goto restore;
      ev->type = BRO_EV_SINFO;
      ev->text = g_strdup (F (4));
      return ev;
    }

restore:
  *colon = ':';
raw:
  memset (ev, 0, sizeof *ev);
  ev->type = BRO_EV_RAW;
  ev->text = g_strdup (line);
  return ev;
}

#undef F

void
bro_event_free (BroEvent *ev)
{
  if (ev == NULL)
    return;
  g_free (ev->text);
  g_free (ev->format);
  if (ev->params)
    g_ptr_array_unref (ev->params);
  g_free (ev->drive.drive_name);
  g_free (ev->drive.disc_name);
  g_free (ev->drive.device);
  g_free (ev);
}

BroSeverity
bro_message_severity (int code, int flags, const char *text)
{
  static const int errors[] = { 2003, 2004, 2023, 5003, 5010, 5021, 5037, 5055, 5069, 5077 };
  static const int warnings[] = { 3038, 3041, 5042 };

  if (code == 1003 || ((flags & 0x20) && text && g_str_has_prefix (text, "DEBUG")))
    return BRO_SEV_DEBUG;
  if (flags & 0x200)
    return BRO_SEV_ERROR;
  for (guint i = 0; i < G_N_ELEMENTS (errors); i++)
    if (errors[i] == code)
      return BRO_SEV_ERROR;
  if (flags & 0x400)
    return BRO_SEV_WARNING;
  for (guint i = 0; i < G_N_ELEMENTS (warnings); i++)
    if (warnings[i] == code)
      return BRO_SEV_WARNING;
  return BRO_SEV_INFO;
}

BroSeverity
bro_event_severity (const BroEvent *ev)
{
  if (ev->type != BRO_EV_MESSAGE)
    return BRO_SEV_INFO;
  return bro_message_severity (ev->code, ev->flags, ev->text);
}

const char *
bro_severity_name (BroSeverity s)
{
  switch (s)
    {
    case BRO_SEV_DEBUG: return "DEBUG";
    case BRO_SEV_WARNING: return "WARNING";
    case BRO_SEV_ERROR: return "ERROR";
    default: return "INFO";
    }
}

BroDriveEntry *
bro_drive_entry_copy (const BroDriveEntry *e)
{
  BroDriveEntry *c = g_new0 (BroDriveEntry, 1);
  c->index = e->index;
  c->state = e->state;
  c->flags = e->flags;
  c->drive_name = g_strdup (e->drive_name ? e->drive_name : "");
  c->disc_name = g_strdup (e->disc_name ? e->disc_name : "");
  c->device = g_strdup (e->device ? e->device : "");
  return c;
}

void
bro_drive_entry_free (BroDriveEntry *e)
{
  if (!e)
    return;
  g_free (e->drive_name);
  g_free (e->disc_name);
  g_free (e->device);
  g_free (e);
}

gboolean
bro_drive_entry_present (const BroDriveEntry *e)
{
  return e->state != BRO_DRIVE_NO_DRIVE && !((!e->drive_name || !*e->drive_name) && (!e->device || !*e->device));
}

char *
bro_drive_entry_lane (const BroDriveEntry *e)
{
  if (e->device && *e->device)
    return g_strdup_printf ("dev:%s", e->device);
  return g_strdup_printf ("disc:%d", e->index);
}

char *
bro_drive_short_model (const char *drive_name)
{
  g_auto (GStrv) parts = g_strsplit_set (drive_name ? drive_name : "", " \t", -1);
  GPtrArray *words = g_ptr_array_new ();
  GString *s = g_string_new (NULL);
  for (int i = 0; parts[i]; i++)
    if (*parts[i])
      g_ptr_array_add (words, parts[i]);
  if (words->len <= 2)
    g_string_append (s, drive_name && *drive_name ? drive_name : "Drive");
  else
    for (guint i = 1; i < words->len && i <= 3; i++)
      g_string_append_printf (s, "%s%s", i > 1 ? " " : "", (char *) words->pdata[i]);
  g_ptr_array_unref (words);
  return g_string_free (s, FALSE);
}

static const char *attribute_names[BRO_ATTR_MAX] = {
  [BRO_ATTR_TYPE] = "Type", [BRO_ATTR_NAME] = "Name", [BRO_ATTR_LANG_CODE] = "Language code",
  [BRO_ATTR_LANG_NAME] = "Language", [BRO_ATTR_CODEC_ID] = "Codec ID", [BRO_ATTR_CODEC_SHORT] = "Codec",
  [BRO_ATTR_CODEC_LONG] = "Codec (long)", [BRO_ATTR_CHAPTER_COUNT] = "Chapters", [BRO_ATTR_DURATION] = "Duration",
  [BRO_ATTR_DISK_SIZE] = "Size", [BRO_ATTR_DISK_SIZE_BYTES] = "Size (bytes)",
  [BRO_ATTR_STREAM_TYPE_EXTENSION] = "Stream type extension", [BRO_ATTR_BITRATE] = "Bitrate",
  [BRO_ATTR_AUDIO_CHANNELS] = "Channels", [BRO_ATTR_ANGLE_INFO] = "Angle", [BRO_ATTR_SOURCE_FILE_NAME] = "Source file",
  [BRO_ATTR_AUDIO_SAMPLE_RATE] = "Sample rate", [BRO_ATTR_AUDIO_SAMPLE_SIZE] = "Sample size",
  [BRO_ATTR_VIDEO_SIZE] = "Resolution", [BRO_ATTR_VIDEO_ASPECT] = "Aspect ratio", [BRO_ATTR_VIDEO_FRAME_RATE] = "Frame rate",
  [BRO_ATTR_STREAM_FLAGS] = "Stream flags", [BRO_ATTR_DATE_TIME] = "Date", [BRO_ATTR_ORIGINAL_TITLE_ID] = "Source title ID",
  [BRO_ATTR_SEGMENTS_COUNT] = "Segment count", [BRO_ATTR_SEGMENTS_MAP] = "Segment map",
  [BRO_ATTR_OUTPUT_FILE_NAME] = "Output file name", [BRO_ATTR_METADATA_LANG_CODE] = "Metadata language code",
  [BRO_ATTR_METADATA_LANG_NAME] = "Metadata language", [BRO_ATTR_TREE_INFO] = "Summary", [BRO_ATTR_PANEL_TITLE] = "Panel title",
  [BRO_ATTR_VOLUME_NAME] = "Volume name", [BRO_ATTR_ORDER_WEIGHT] = "Order weight", [BRO_ATTR_OUTPUT_FORMAT] = "Output format",
  [BRO_ATTR_OUTPUT_FORMAT_DESCRIPTION] = "Output format description", [BRO_ATTR_SEAMLESS_INFO] = "Seamless info",
  [BRO_ATTR_PANEL_TEXT] = "Panel text", [BRO_ATTR_MKV_FLAGS] = "MKV flags", [BRO_ATTR_MKV_FLAGS_TEXT] = "MKV flags (text)",
  [BRO_ATTR_AUDIO_CHANNEL_LAYOUT_NAME] = "Channel layout", [BRO_ATTR_OUTPUT_CODEC_SHORT] = "Output codec",
  [BRO_ATTR_OUTPUT_CONVERSION_TYPE] = "Conversion", [BRO_ATTR_OUTPUT_AUDIO_SAMPLE_RATE] = "Output sample rate",
  [BRO_ATTR_OUTPUT_AUDIO_SAMPLE_SIZE] = "Output sample size", [BRO_ATTR_OUTPUT_AUDIO_CHANNELS] = "Output channels",
  [BRO_ATTR_OUTPUT_AUDIO_CHANNEL_LAYOUT_NAME] = "Output channel layout",
  [BRO_ATTR_OUTPUT_AUDIO_CHANNEL_LAYOUT] = "Output channel layout ID",
  [BRO_ATTR_OUTPUT_AUDIO_MIX_DESCRIPTION] = "Output mix", [BRO_ATTR_COMMENT] = "Comment",
  [BRO_ATTR_OFFSET_SEQUENCE_ID] = "Offset sequence ID",
};

/* camelCase keys used in disc-info.json, identical to the other front-ends. */
static const char *attribute_keys[BRO_ATTR_MAX] = {
  [BRO_ATTR_TYPE] = "type", [BRO_ATTR_NAME] = "name", [BRO_ATTR_LANG_CODE] = "langCode", [BRO_ATTR_LANG_NAME] = "langName",
  [BRO_ATTR_CODEC_ID] = "codecId", [BRO_ATTR_CODEC_SHORT] = "codecShort", [BRO_ATTR_CODEC_LONG] = "codecLong",
  [BRO_ATTR_CHAPTER_COUNT] = "chapterCount", [BRO_ATTR_DURATION] = "duration", [BRO_ATTR_DISK_SIZE] = "diskSize",
  [BRO_ATTR_DISK_SIZE_BYTES] = "diskSizeBytes", [BRO_ATTR_STREAM_TYPE_EXTENSION] = "streamTypeExtension",
  [BRO_ATTR_BITRATE] = "bitrate", [BRO_ATTR_AUDIO_CHANNELS] = "audioChannelsCount", [BRO_ATTR_ANGLE_INFO] = "angleInfo",
  [BRO_ATTR_SOURCE_FILE_NAME] = "sourceFileName", [BRO_ATTR_AUDIO_SAMPLE_RATE] = "audioSampleRate",
  [BRO_ATTR_AUDIO_SAMPLE_SIZE] = "audioSampleSize", [BRO_ATTR_VIDEO_SIZE] = "videoSize",
  [BRO_ATTR_VIDEO_ASPECT] = "videoAspectRatio", [BRO_ATTR_VIDEO_FRAME_RATE] = "videoFrameRate",
  [BRO_ATTR_STREAM_FLAGS] = "streamFlags", [BRO_ATTR_DATE_TIME] = "dateTime", [BRO_ATTR_ORIGINAL_TITLE_ID] = "originalTitleId",
  [BRO_ATTR_SEGMENTS_COUNT] = "segmentsCount", [BRO_ATTR_SEGMENTS_MAP] = "segmentsMap",
  [BRO_ATTR_OUTPUT_FILE_NAME] = "outputFileName", [BRO_ATTR_METADATA_LANG_CODE] = "metadataLanguageCode",
  [BRO_ATTR_METADATA_LANG_NAME] = "metadataLanguageName", [BRO_ATTR_TREE_INFO] = "treeInfo",
  [BRO_ATTR_PANEL_TITLE] = "panelTitle", [BRO_ATTR_VOLUME_NAME] = "volumeName", [BRO_ATTR_ORDER_WEIGHT] = "orderWeight",
  [BRO_ATTR_OUTPUT_FORMAT] = "outputFormat", [BRO_ATTR_OUTPUT_FORMAT_DESCRIPTION] = "outputFormatDescription",
  [BRO_ATTR_SEAMLESS_INFO] = "seamlessInfo", [BRO_ATTR_PANEL_TEXT] = "panelText", [BRO_ATTR_MKV_FLAGS] = "mkvFlags",
  [BRO_ATTR_MKV_FLAGS_TEXT] = "mkvFlagsText", [BRO_ATTR_AUDIO_CHANNEL_LAYOUT_NAME] = "audioChannelLayoutName",
  [BRO_ATTR_OUTPUT_CODEC_SHORT] = "outputCodecShort", [BRO_ATTR_OUTPUT_CONVERSION_TYPE] = "outputConversionType",
  [BRO_ATTR_OUTPUT_AUDIO_SAMPLE_RATE] = "outputAudioSampleRate", [BRO_ATTR_OUTPUT_AUDIO_SAMPLE_SIZE] = "outputAudioSampleSize",
  [BRO_ATTR_OUTPUT_AUDIO_CHANNELS] = "outputAudioChannelsCount",
  [BRO_ATTR_OUTPUT_AUDIO_CHANNEL_LAYOUT_NAME] = "outputAudioChannelLayoutName",
  [BRO_ATTR_OUTPUT_AUDIO_CHANNEL_LAYOUT] = "outputAudioChannelLayout",
  [BRO_ATTR_OUTPUT_AUDIO_MIX_DESCRIPTION] = "outputAudioMixDescription", [BRO_ATTR_COMMENT] = "comment",
  [BRO_ATTR_OFFSET_SEQUENCE_ID] = "offsetSequenceId",
};

const char *
bro_attribute_name (int id)
{
  if (id > 0 && id < BRO_ATTR_MAX && attribute_names[id])
    return attribute_names[id];
  return NULL;
}

gboolean
bro_attribute_visible (int id)
{
  switch (id)
    {
    case 0:
    case BRO_ATTR_PANEL_TITLE:
    case BRO_ATTR_PANEL_TEXT:
    case BRO_ATTR_TREE_INFO:
    case BRO_ATTR_ORDER_WEIGHT:
    case BRO_ATTR_STREAM_TYPE_EXTENSION:
    case BRO_ATTR_OUTPUT_AUDIO_CHANNEL_LAYOUT:
      return FALSE;
    default:
      return TRUE;
    }
}

const char *
bro_drive_state_name (BroDriveState s)
{
  switch (s)
    {
    case BRO_DRIVE_EMPTY_CLOSED: return "No disc";
    case BRO_DRIVE_EMPTY_OPEN: return "Tray open";
    case BRO_DRIVE_INSERTED: return "Disc inserted";
    case BRO_DRIVE_LOADING: return "Loading…";
    case BRO_DRIVE_UNMOUNTING: return "Unmounting…";
    default: return "Not present";
    }
}

const char *
bro_disc_type_name (int flags)
{
  if (flags & BRO_DISC_BLURAY)
    return (flags & BRO_DISC_AACS) ? "Blu-ray (AACS)" : "Blu-ray";
  if (flags & BRO_DISC_HDDVD)
    return "HD DVD";
  if (flags & BRO_DISC_DVD)
    return "DVD";
  return "Disc";
}

char *
bro_stream_flags_describe (int f)
{
  GString *s = g_string_new (NULL);
#define ADD(bit, text) if (f & (bit)) g_string_append_printf (s, "%s%s", s->len ? ", " : "", text)
  ADD (1, "Director's comments");
  ADD (2, "Alternate director's comments");
  ADD (4, "For visually impaired");
  ADD (256, "Core audio");
  ADD (512, "Secondary audio");
  ADD (1024, "Has core audio");
  ADD (2048, "Derived stream");
  ADD (4096, "Forced subtitles");
#undef ADD
  return g_string_free (s, FALSE);
}

/* ---- disc model ---- */

static GHashTable *
attr_table_new (void)
{
  return g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}

static void
track_free (BroTrack *t)
{
  g_hash_table_unref (t->attrs);
  g_free (t);
}

static void
title_free (BroTitle *t)
{
  g_hash_table_unref (t->attrs);
  g_ptr_array_unref (t->tracks);
  g_free (t);
}

BroDiscInfo *
bro_disc_info_new (void)
{
  BroDiscInfo *d = g_new0 (BroDiscInfo, 1);
  g_atomic_ref_count_init (&d->ref);
  d->attrs = attr_table_new ();
  d->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) title_free);
  return d;
}

BroDiscInfo *
bro_disc_info_ref (BroDiscInfo *info)
{
  g_atomic_ref_count_inc (&info->ref);
  return info;
}

void
bro_disc_info_unref (BroDiscInfo *info)
{
  if (info && g_atomic_ref_count_dec (&info->ref))
    {
      g_hash_table_unref (info->attrs);
      g_ptr_array_unref (info->titles);
      g_free (info);
    }
}

static int
compare_title (gconstpointer a, gconstpointer b)
{
  return (*(BroTitle **) a)->index - (*(BroTitle **) b)->index;
}

static int
compare_track (gconstpointer a, gconstpointer b)
{
  return (*(BroTrack **) a)->index - (*(BroTrack **) b)->index;
}

BroTitle *
bro_disc_info_title (BroDiscInfo *info, int index)
{
  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      if (t->index == index)
        return t;
    }
  return NULL;
}

static BroTitle *
ensure_title (BroDiscInfo *info, int index)
{
  BroTitle *t = bro_disc_info_title (info, index);
  if (t)
    return t;
  t = g_new0 (BroTitle, 1);
  t->index = index;
  t->attrs = attr_table_new ();
  t->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) track_free);
  g_ptr_array_add (info->titles, t);
  g_ptr_array_sort (info->titles, compare_title);
  return t;
}

void
bro_disc_info_consume (BroDiscInfo *info, const BroEvent *ev)
{
  switch (ev->type)
    {
    case BRO_EV_TITLE_COUNT:
      info->reported_title_count = ev->total;
      break;
    case BRO_EV_CINFO:
      g_hash_table_replace (info->attrs, GINT_TO_POINTER (ev->id), g_strdup (ev->text));
      break;
    case BRO_EV_TINFO:
      g_hash_table_replace (ensure_title (info, ev->title)->attrs, GINT_TO_POINTER (ev->id), g_strdup (ev->text));
      break;
    case BRO_EV_SINFO:
      {
        BroTitle *t = ensure_title (info, ev->title);
        BroTrack *tr = NULL;
        for (guint i = 0; i < t->tracks->len; i++)
          if (((BroTrack *) t->tracks->pdata[i])->index == ev->stream)
            tr = t->tracks->pdata[i];
        if (!tr)
          {
            tr = g_new0 (BroTrack, 1);
            tr->index = ev->stream;
            tr->attrs = attr_table_new ();
            g_ptr_array_add (t->tracks, tr);
            g_ptr_array_sort (t->tracks, compare_track);
          }
        g_hash_table_replace (tr->attrs, GINT_TO_POINTER (ev->id), g_strdup (ev->text));
      }
      break;
    default:
      break;
    }
}

BroDiscInfo *
bro_disc_info_from_output (const char *text)
{
  BroDiscInfo *info = bro_disc_info_new ();
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (int i = 0; lines[i]; i++)
    {
      g_autoptr (BroEvent) ev = bro_event_parse (lines[i]);
      if (ev)
        bro_disc_info_consume (info, ev);
    }
  return info;
}

const char *
bro_disc_info_attr (BroDiscInfo *info, int id)
{
  return g_hash_table_lookup (info->attrs, GINT_TO_POINTER (id));
}

const char *
bro_disc_info_name (BroDiscInfo *info)
{
  const char *n = bro_disc_info_attr (info, BRO_ATTR_NAME);
  if (!n)
    n = bro_disc_info_attr (info, BRO_ATTR_VOLUME_NAME);
  return n ? n : "";
}

const char *
bro_disc_info_type_token (BroDiscInfo *info)
{
  const char *t = info ? bro_disc_info_attr (info, BRO_ATTR_TYPE) : NULL;
  g_autofree char *lower = g_ascii_strdown (t ? t : "", -1);
  if (strstr (lower, "blu"))
    return "bd";
  if (strstr (lower, "hd"))
    return "hddvd";
  if (strstr (lower, "dvd"))
    return "dvd";
  return "disc";
}

static JsonNode *
attrs_to_json (GHashTable *attrs)
{
  JsonObject *o = json_object_new ();
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, attrs);
  while (g_hash_table_iter_next (&it, &k, &v))
    {
      int id = GPOINTER_TO_INT (k);
      g_autofree char *fallback = g_strdup_printf ("attr%d", id);
      const char *key = (id > 0 && id < BRO_ATTR_MAX && attribute_keys[id]) ? attribute_keys[id] : fallback;
      json_object_set_string_member (o, key, v);
    }
  JsonNode *n = json_node_new (JSON_NODE_OBJECT);
  json_node_take_object (n, o);
  return n;
}

char *
bro_disc_info_to_json (BroDiscInfo *info)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "attributes");
  json_builder_add_value (b, attrs_to_json (info->attrs));
  json_builder_set_member_name (b, "titles");
  json_builder_begin_array (b);
  for (guint i = 0; i < info->titles->len; i++)
    {
      BroTitle *t = info->titles->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "index");
      json_builder_add_int_value (b, t->index);
      json_builder_set_member_name (b, "attributes");
      json_builder_add_value (b, attrs_to_json (t->attrs));
      json_builder_set_member_name (b, "tracks");
      json_builder_begin_array (b);
      for (guint j = 0; j < t->tracks->len; j++)
        {
          BroTrack *tr = t->tracks->pdata[j];
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "index");
          json_builder_add_int_value (b, tr->index);
          json_builder_set_member_name (b, "attributes");
          json_builder_add_value (b, attrs_to_json (tr->attrs));
          json_builder_end_object (b);
        }
      json_builder_end_array (b);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  json_generator_set_pretty (gen, TRUE);
  return json_generator_to_data (gen, NULL);
}

const char *
bro_title_attr (BroTitle *t, int id)
{
  return g_hash_table_lookup (t->attrs, GINT_TO_POINTER (id));
}

const char *
bro_title_str (BroTitle *t, int id)
{
  const char *v = bro_title_attr (t, id);
  return v ? v : "";
}

int
bro_parse_duration (const char *s)
{
  int total = 0;
  g_auto (GStrv) parts = g_strsplit (s ? s : "", ":", -1);
  for (int i = 0; parts[i]; i++)
    total = total * 60 + atoi (g_strstrip (parts[i]));
  return total;
}

char *
bro_format_duration (int seconds)
{
  return g_strdup_printf ("%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

char *
bro_format_bytes (gint64 bytes)
{
  return g_format_size ((guint64) MAX (bytes, 0));
}

int
bro_title_duration (BroTitle *t)
{
  return bro_parse_duration (bro_title_attr (t, BRO_ATTR_DURATION));
}

static int
attr_int (GHashTable *attrs, int id, int fallback)
{
  const char *v = g_hash_table_lookup (attrs, GINT_TO_POINTER (id));
  int out;
  return (v && parse_int (v, &out)) ? out : fallback;
}

int
bro_title_chapters (BroTitle *t)
{
  return attr_int (t->attrs, BRO_ATTR_CHAPTER_COUNT, 0);
}

gint64
bro_title_size (BroTitle *t)
{
  const char *v = bro_title_attr (t, BRO_ATTR_DISK_SIZE_BYTES);
  return v ? g_ascii_strtoll (v, NULL, 10) : 0;
}

int
bro_title_source_id (BroTitle *t)
{
  return attr_int (t->attrs, BRO_ATTR_ORIGINAL_TITLE_ID, -1);
}

int
bro_title_angle (BroTitle *t)
{
  return attr_int (t->attrs, BRO_ATTR_ANGLE_INFO, -1);
}

const char *
bro_track_attr (BroTrack *t, int id)
{
  return g_hash_table_lookup (t->attrs, GINT_TO_POINTER (id));
}

BroTrackKind
bro_track_kind (BroTrack *t)
{
  const char *ty = bro_track_attr (t, BRO_ATTR_TYPE);
  if (!ty)
    return BRO_TRACK_OTHER;
  if (g_ascii_strcasecmp (ty, "video") == 0)
    return BRO_TRACK_VIDEO;
  if (g_ascii_strcasecmp (ty, "audio") == 0)
    return BRO_TRACK_AUDIO;
  if (g_ascii_strcasecmp (ty, "subtitles") == 0 || g_ascii_strcasecmp (ty, "subtitle") == 0)
    return BRO_TRACK_SUBTITLE;
  if (g_ascii_strcasecmp (ty, "attachment") == 0)
    return BRO_TRACK_ATTACHMENT;
  return BRO_TRACK_OTHER;
}

gboolean
bro_track_is_default (BroTrack *t)
{
  const char *f = bro_track_attr (t, BRO_ATTR_MKV_FLAGS);
  return f && strchr (f, 'd') != NULL;
}

int
bro_track_flags (BroTrack *t)
{
  return attr_int (t->attrs, BRO_ATTR_STREAM_FLAGS, 0);
}

char *
bro_track_summary (BroTrack *t)
{
  const char *tree = bro_track_attr (t, BRO_ATTR_TREE_INFO);
  GString *s;
  if (tree)
    {
      g_autofree char *copy = g_strstrip (g_strdup (tree));
      if (*copy)
        return g_steal_pointer (&copy);
    }
  s = g_string_new (NULL);
  const int ids[] = { BRO_ATTR_CODEC_SHORT, BRO_ATTR_NAME, BRO_ATTR_LANG_NAME };
  for (guint i = 0; i < G_N_ELEMENTS (ids); i++)
    {
      const char *v = bro_track_attr (t, ids[i]);
      if (v && *v)
        g_string_append_printf (s, "%s%s", s->len ? " " : "", v);
    }
  return g_string_free (s, FALSE);
}
