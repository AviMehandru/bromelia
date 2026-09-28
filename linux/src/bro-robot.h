/* bro-robot.h — makemkvcon robot (-r) protocol parser and disc model.
 *
 * Shared behaviour with the macOS (Swift) and Windows (C#) versions; see docs/robot-protocol.md.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Attribute identifiers (MakeMKV apdefs.h). */
enum {
  BRO_ATTR_TYPE = 1, BRO_ATTR_NAME = 2, BRO_ATTR_LANG_CODE = 3, BRO_ATTR_LANG_NAME = 4, BRO_ATTR_CODEC_ID = 5,
  BRO_ATTR_CODEC_SHORT = 6, BRO_ATTR_CODEC_LONG = 7, BRO_ATTR_CHAPTER_COUNT = 8, BRO_ATTR_DURATION = 9,
  BRO_ATTR_DISK_SIZE = 10, BRO_ATTR_DISK_SIZE_BYTES = 11, BRO_ATTR_STREAM_TYPE_EXTENSION = 12, BRO_ATTR_BITRATE = 13,
  BRO_ATTR_AUDIO_CHANNELS = 14, BRO_ATTR_ANGLE_INFO = 15, BRO_ATTR_SOURCE_FILE_NAME = 16, BRO_ATTR_AUDIO_SAMPLE_RATE = 17,
  BRO_ATTR_AUDIO_SAMPLE_SIZE = 18, BRO_ATTR_VIDEO_SIZE = 19, BRO_ATTR_VIDEO_ASPECT = 20, BRO_ATTR_VIDEO_FRAME_RATE = 21,
  BRO_ATTR_STREAM_FLAGS = 22, BRO_ATTR_DATE_TIME = 23, BRO_ATTR_ORIGINAL_TITLE_ID = 24, BRO_ATTR_SEGMENTS_COUNT = 25,
  BRO_ATTR_SEGMENTS_MAP = 26, BRO_ATTR_OUTPUT_FILE_NAME = 27, BRO_ATTR_METADATA_LANG_CODE = 28,
  BRO_ATTR_METADATA_LANG_NAME = 29, BRO_ATTR_TREE_INFO = 30, BRO_ATTR_PANEL_TITLE = 31, BRO_ATTR_VOLUME_NAME = 32,
  BRO_ATTR_ORDER_WEIGHT = 33, BRO_ATTR_OUTPUT_FORMAT = 34, BRO_ATTR_OUTPUT_FORMAT_DESCRIPTION = 35,
  BRO_ATTR_SEAMLESS_INFO = 36, BRO_ATTR_PANEL_TEXT = 37, BRO_ATTR_MKV_FLAGS = 38, BRO_ATTR_MKV_FLAGS_TEXT = 39,
  BRO_ATTR_AUDIO_CHANNEL_LAYOUT_NAME = 40, BRO_ATTR_OUTPUT_CODEC_SHORT = 41, BRO_ATTR_OUTPUT_CONVERSION_TYPE = 42,
  BRO_ATTR_OUTPUT_AUDIO_SAMPLE_RATE = 43, BRO_ATTR_OUTPUT_AUDIO_SAMPLE_SIZE = 44, BRO_ATTR_OUTPUT_AUDIO_CHANNELS = 45,
  BRO_ATTR_OUTPUT_AUDIO_CHANNEL_LAYOUT_NAME = 46, BRO_ATTR_OUTPUT_AUDIO_CHANNEL_LAYOUT = 47,
  BRO_ATTR_OUTPUT_AUDIO_MIX_DESCRIPTION = 48, BRO_ATTR_COMMENT = 49, BRO_ATTR_OFFSET_SEQUENCE_ID = 50,
  BRO_ATTR_MAX = 51
};

typedef enum {
  BRO_DRIVE_EMPTY_CLOSED = 0,
  BRO_DRIVE_EMPTY_OPEN = 1,
  BRO_DRIVE_INSERTED = 2,
  BRO_DRIVE_LOADING = 3,
  BRO_DRIVE_NO_DRIVE = 256,
  BRO_DRIVE_UNMOUNTING = 257,
} BroDriveState;

enum {
  BRO_DISC_DVD = 1,
  BRO_DISC_HDDVD = 2,
  BRO_DISC_BLURAY = 4,
  BRO_DISC_AACS = 8,
  BRO_DISC_BDSVM = 16,
};

typedef enum { BRO_SEV_DEBUG, BRO_SEV_INFO, BRO_SEV_WARNING, BRO_SEV_ERROR } BroSeverity;

typedef enum {
  BRO_EV_MESSAGE,
  BRO_EV_PROGRESS_CURRENT, /* PRGC */
  BRO_EV_PROGRESS_TOTAL,   /* PRGT */
  BRO_EV_PROGRESS_VALUE,   /* PRGV */
  BRO_EV_DRIVE,            /* DRV */
  BRO_EV_TITLE_COUNT,      /* TCOUNT */
  BRO_EV_CINFO,
  BRO_EV_TINFO,
  BRO_EV_SINFO,
  BRO_EV_RAW,
} BroEventType;

typedef struct {
  int index;
  BroDriveState state;
  int flags;
  char *drive_name;
  char *disc_name;
  char *device;
} BroDriveEntry;

typedef struct {
  BroEventType type;
  int code;          /* MSG code, PRGC/PRGT code, xINFO code */
  int flags;         /* MSG flags */
  int id;            /* PRGC/PRGT id, xINFO attribute id */
  int title, stream; /* TINFO / SINFO */
  int current, total, max; /* PRGV; TCOUNT uses total */
  char *text;        /* message text, progress name, attribute value, raw line */
  char *format;      /* MSG format string */
  GPtrArray *params; /* MSG parameters (char*) */
  BroDriveEntry drive;
} BroEvent;

GPtrArray  *bro_split_fields (const char *s);
BroEvent   *bro_event_parse (const char *line);
void        bro_event_free (BroEvent *ev);
BroSeverity bro_message_severity (int code, int flags, const char *text);
BroSeverity bro_event_severity (const BroEvent *ev);
const char *bro_severity_name (BroSeverity s);

BroDriveEntry *bro_drive_entry_copy (const BroDriveEntry *e);
void           bro_drive_entry_free (BroDriveEntry *e);
gboolean       bro_drive_entry_present (const BroDriveEntry *e);
char          *bro_drive_entry_lane (const BroDriveEntry *e);
char          *bro_drive_short_model (const char *drive_name);

const char *bro_attribute_name (int id);
gboolean    bro_attribute_visible (int id);
const char *bro_drive_state_name (BroDriveState s);
const char *bro_disc_type_name (int flags);
char       *bro_stream_flags_describe (int flags);

/* ---- disc model ---- */

typedef enum { BRO_TRACK_VIDEO, BRO_TRACK_AUDIO, BRO_TRACK_SUBTITLE, BRO_TRACK_ATTACHMENT, BRO_TRACK_OTHER } BroTrackKind;

typedef struct {
  int index;
  GHashTable *attrs; /* GINT_TO_POINTER(id) -> char* */
} BroTrack;

typedef struct {
  int index;
  GHashTable *attrs;
  GPtrArray *tracks; /* BroTrack* sorted by index */
} BroTitle;

typedef struct {
  gatomicrefcount ref;
  GHashTable *attrs;
  GPtrArray *titles; /* BroTitle* sorted by index */
  int reported_title_count;
} BroDiscInfo;

BroDiscInfo *bro_disc_info_new (void);
BroDiscInfo *bro_disc_info_ref (BroDiscInfo *info);
void         bro_disc_info_unref (BroDiscInfo *info);
void         bro_disc_info_consume (BroDiscInfo *info, const BroEvent *ev);
BroDiscInfo *bro_disc_info_from_output (const char *text);
BroTitle    *bro_disc_info_title (BroDiscInfo *info, int index);
const char  *bro_disc_info_attr (BroDiscInfo *info, int id);
const char  *bro_disc_info_name (BroDiscInfo *info);
const char  *bro_disc_info_type_token (BroDiscInfo *info);
char        *bro_disc_info_to_json (BroDiscInfo *info);

const char  *bro_title_attr (BroTitle *t, int id);
int          bro_title_duration (BroTitle *t);
int          bro_title_chapters (BroTitle *t);
gint64       bro_title_size (BroTitle *t);
int          bro_title_source_id (BroTitle *t); /* -1 when unknown */
int          bro_title_angle (BroTitle *t);     /* -1 when unknown */
const char  *bro_title_str (BroTitle *t, int id); /* "" when missing */

const char  *bro_track_attr (BroTrack *t, int id);
BroTrackKind bro_track_kind (BroTrack *t);
gboolean     bro_track_is_default (BroTrack *t);
int          bro_track_flags (BroTrack *t);
char        *bro_track_summary (BroTrack *t);

int   bro_parse_duration (const char *s);
char *bro_format_duration (int seconds);
char *bro_format_bytes (gint64 bytes);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroEvent, bro_event_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDiscInfo, bro_disc_info_unref)

/* Messages about the drive and about MakeMKV itself that Bromelia shows outside the log. */
typedef enum {
  BRO_NOTICE_NONE,
  BRO_NOTICE_LIBREDRIVE,             /* "Using LibreDrive mode (v06.3 id=…)" */
  BRO_NOTICE_LIBREDRIVE_REQUIRED,    /* the disc (4K UHD) needs a LibreDrive-compatible drive */
  BRO_NOTICE_KEY_EXPIRED,            /* evaluation period / beta key expired (5052, 5055) */
  BRO_NOTICE_EVALUATION_NOT_STARTED, /* makemkvcon can't start the evaluation */
  BRO_NOTICE_VERSION_TOO_OLD,        /* "This application version is too old" */
} BroNotice;

/* The notice a message carries; for LibreDrive, *detail (optional) receives "v06.3 id=…". */
BroNotice   bro_notice_from_event (const BroEvent *ev, char **detail);
gboolean    bro_notice_is_license_problem (BroNotice n);
const char *bro_notice_explanation (BroNotice n);

G_END_DECLS
