/* bro-web.h — the web page (shared/web/bromelia-web.html) and its JSON API, for watching and controlling Bromelia
 * from a browser. Same API and access rules as the macOS and Windows versions. */
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "bro-config.h"

G_BEGIN_DECLS

typedef struct {
  char *method; /* upper case */
  char *path;   /* still percent-encoded */
  GHashTable *query;   /* char* -> char* */
  GHashTable *headers; /* lower-case name -> value */
} BroHttpRequest;

/* The request head in data; NULL until the blank line has arrived, and for malformed requests. */
BroHttpRequest *bro_http_request_parse (const char *data, gsize len);
void            bro_http_request_free (BroHttpRequest *r);

/* Who may use the page: with a token, whoever sends it (Authorization: Bearer or ?token=); without one, only pages on
 * this computer (the Host header must be localhost, which also defeats DNS rebinding). POST needs X-Bromelia: 1.
 * Returns 200, or 401 / 403 with *why. */
int   bro_web_access (const BroHttpRequest *r, const BroWebUIConfig *config, const char **why);
/* Why the page can't be started with config (the network without a token, a bad port), or NULL. */
char *bro_web_start_problem (const BroWebUIConfig *config);
GBytes *bro_web_page (void); /* the page, from the resources */

/* status: the JSON for GET /api/status (new reference). action: runs drives/<lane>/open|rip|eject|close,
 * jobs/<id>/cancel, settings/<id>/set or verify (query: the request's parameters) and returns an error message, or NULL.
 * log: the end of a job's log for GET /api/jobs/<id>/log, or NULL for an unknown job. */
typedef JsonNode *(*BroWebStatusFunc) (gpointer user_data);
typedef char *(*BroWebActionFunc) (const char *kind, const char *id, const char *action, GHashTable *query, gpointer user_data);
typedef char *(*BroWebLogFunc) (const char *id, gpointer user_data);

typedef struct _BroWebServer BroWebServer;

/* The end of a log file: at most limit bytes, from the start of a line ("…" first when cut). */
char         *bro_web_log_tail (const char *path, gsize limit);

BroWebServer *bro_web_server_new (BroWebStatusFunc status, BroWebActionFunc action, BroWebLogFunc log, gpointer user_data);
void          bro_web_server_free (BroWebServer *s);
/* Starts, restarts or stops the server to match config (on the main thread). */
void          bro_web_server_apply (BroWebServer *s, const BroWebUIConfig *config);
void          bro_web_server_stop (BroWebServer *s);
gboolean      bro_web_server_running (BroWebServer *s);
const char   *bro_web_server_last_error (BroWebServer *s); /* NULL when fine */
/* The answer to a request: status code, content type and body (for tests; the server uses it too). */
int           bro_web_server_handle (BroWebServer *s, const BroHttpRequest *r, const char **content_type, GBytes **body);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroHttpRequest, bro_http_request_free)

G_END_DECLS
