/* bro-integrations.h — online services (notifications, TMDb / OMDb lookup), media server names, and discs that
 * MakeMKV doesn't handle (audio CDs, data discs). Same rules as the macOS and Windows versions. */
#pragma once

#include <gio/gio.h>
#include "bro-config.h"
#include "bro-identity.h"

G_BEGIN_DECLS

/* ---- HTTP (with curl; secrets go through a private config file, never the command line) ---- */

/* headers: NULL-terminated "Name: value" strings. Returns FALSE (with error) when curl couldn't be run or the
 * request failed before an answer; *status receives the HTTP status and *response (optional) the body. */
gboolean bro_http_request (const char *method, const char *url, const char *const *headers, const char *body, gsize body_len,
                           int timeout_seconds, int *status, GString *response, GError **error);

/* ---- notifications ---- */

typedef struct {
  char *url;          /* HTTP POST target, or NULL for an Apprise URL */
  GPtrArray *headers; /* char* "Name: value" */
  char *body;
  char *apprise_url;  /* sent with the apprise command */
} BroDelivery;

/* How target is reached (Discord / Slack webhooks, ntfy, a generic JSON webhook, else Apprise); NULL when unusable.
 * status is success, errors, failed or cancelled. */
BroDelivery *bro_delivery_for (const char *target, const char *title, const char *body, const char *status);
void         bro_delivery_free (BroDelivery *d);

typedef void (*BroTextFunc) (const char *text, gpointer user_data);
/* Sends to every enabled target that wants this status (blocking). Failures are passed to log. */
void bro_notifications_send (GPtrArray *targets, const char *title, const char *body, const char *status,
                             BroTextFunc log, gpointer user_data);

/* ---- online lookup ---- */

typedef struct {
  char *title;
  int year;      /* 0 when unknown */
  int tmdb_id;   /* 0 when unknown */
  char *imdb_id; /* NULL when unknown */
  const char *provider; /* "TMDb" / "OMDb" */
} BroMediaMatch;

void           bro_media_match_free (BroMediaMatch *m);
/* The request URL and, for a TMDb read access token, the bearer token (*bearer, else NULL). FALSE without a key or name. */
gboolean       bro_metadata_request (const char *name, BroMediaKind kind, const BroMetadataConfig *config, char **url, char **bearer);
/* The result whose title matches name (ignoring case and punctuation), else the first one. */
BroMediaMatch *bro_metadata_parse (const char *json, BroMetadataProvider provider, const char *name);
/* Blocking. NULL with error on failure, NULL without error when nothing was found. */
BroMediaMatch *bro_metadata_lookup (const char *name, BroMediaKind kind, const BroMetadataConfig *config, GError **error);

/* ---- media server layout (Plex / Jellyfin / Emby) ---- */

#define BRO_MEDIA_FOLDER_TEMPLATE "{libraryFolder}/{name}{releaseYear? ({releaseYear})}"
#define BRO_MEDIA_MAIN_TEMPLATE "{name}{releaseYear? ({releaseYear})}"
#define BRO_MEDIA_EPISODE_TEMPLATE "Season {seasonOr1:2}/{name}{releaseYear? ({releaseYear})} - S{seasonOr1:2}E{episodeNumber:2}"
#define BRO_MEDIA_OTHER_TEMPLATE "Other/{name}{releaseYear? ({releaseYear})} - {track}"
#define BRO_MEDIA_BACKUP_NAME "{name}{releaseYear? ({releaseYear})} - Backup - {format}"

/* The file name template for an episode, the main feature of a movie, or another title. */
const char *bro_media_server_file_template (GHashTable *values, gboolean is_main_feature);

/* ---- discs without a DVD / Blu-ray structure ---- */

typedef enum { BRO_CONTENT_VIDEO, BRO_CONTENT_AUDIO, BRO_CONTENT_DATA, BRO_CONTENT_UNKNOWN } BroDiscContent;
typedef BroDiscContent (*BroContentProbe) (const char *device);

/* What udev knows about the disc in device (audio / data tracks, a DVD / Blu-ray structure on the mounted disc). */
BroDiscContent bro_disc_content_probe (const char *device);
/* The mode for an automatic or quick rip: the drive's mode for DVDs and Blu-rays; audio CD / data image for other discs
 * when the drive is set up for them. FALSE: leave the disc alone. probe is only called for discs without video flags. */
gboolean       bro_disc_mode_for (int drive_flags, BroContentProbe probe, const char *device, const BroDriveConfig *drive,
                                  BroRipMode *mode);

/* The audio CD command (the configured one, else cyanrip, else abcde) as argv, or NULL when none is installed. */
GPtrArray *bro_audio_command (const BroOtherDiscs *other, const char *device);

typedef gboolean (*BroCopyProgress) (gint64 done, gint64 total, gpointer user_data); /* FALSE = stop */
/* Copies a disc (or any file) byte for byte to dest, which must not exist. A read error fails the copy. */
gboolean bro_copy_disc (const char *device, const char *dest, BroCopyProgress progress, gpointer user_data,
                        GCancellable *cancellable, GError **error);

/* ---- drive control ---- */

gboolean bro_close_tray (const char *device);    /* eject -t */
gboolean bro_disc_is_mounted (const char *device);
char    *bro_disc_mount_point (const char *device); /* from /proc/self/mounts, or NULL */

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDelivery, bro_delivery_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMediaMatch, bro_media_match_free)

G_END_DECLS
