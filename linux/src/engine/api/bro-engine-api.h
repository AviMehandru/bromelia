/* bro-engine-api.h: BroEngineAPI: the one interface between the engine and anything outside it (plan §13.1): DTOs in
 * and out, never live objects. BroLocalEngine implements it now; RemoteEngine (phase 4) too. It holds the operations of
 * the phases built so far: it grows with each phase. Every operation answers once through @done, on the engine's
 * executor, with what the route lists on failure, server.stopping once the engine stops; a call the executor no longer
 * takes answers server.stopping on the caller's thread. The results are borrowed; @destroy frees @data afterwards. */
#pragma once

#include "bro-config-envelope.h"
#include "bro-event-feed.h"
#include "bro-id.h"
#include "bro-job-detail-dto.h"
#include "bro-job-dto.h"
#include "bro-job-page-dto.h"
#include "bro-job-query-dto.h"
#include "bro-library-dto.h"
#include "bro-rip-options-dto.h"
#include "bro-step-kind.h"
#include "bro-system-status-dto.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_ENGINE_API (bro_engine_api_get_type ())
G_DECLARE_INTERFACE (BroEngineAPI, bro_engine_api, BRO, ENGINE_API, GObject)

/* @error NULL when it succeeded. */
typedef void (*BroEngineDone) (const BroBroError *error, gpointer data);
typedef void (*BroSystemStatusDone) (const BroSystemStatusDto *status, const BroBroError *error, gpointer data);
/* @items: what the operation says (NULL on failure). */
typedef void (*BroEngineListDone) (GPtrArray *items, const BroBroError *error, gpointer data);
typedef void (*BroEngineJobDone) (const BroJobDto *job, const BroBroError *error, gpointer data);
typedef void (*BroEnginePageDone) (const BroJobPageDto *page, const BroBroError *error, gpointer data);
typedef void (*BroEngineDetailDone) (const BroJobDetailDto *detail, const BroBroError *error, gpointer data);
typedef void (*BroEngineLibraryDone) (const BroLibraryDto *library, const BroBroError *error, gpointer data);
typedef void (*BroEngineEnvelopeDone) (const BroConfigEnvelope *envelope, const BroBroError *error, gpointer data);
typedef void (*BroEngineValueDone) (const BroJsonValue *value, const BroBroError *error, gpointer data);
/* @feed: borrowed (ref it to keep it; close it when done). */
typedef void (*BroEngineFeedDone) (BroEventFeed *feed, const BroBroError *error, gpointer data);

struct _BroEngineAPIInterface {
  GTypeInterface parent_iface;
  void (*system_status) (BroEngineAPI *self, BroSystemStatusDone done, gpointer data, GDestroyNotify destroy);
  void (*stop) (BroEngineAPI *self, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*drives) (BroEngineAPI *self, BroEngineListDone done, gpointer data, GDestroyNotify destroy);
  void (*eject) (BroEngineAPI *self, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*close_tray) (BroEngineAPI *self, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*close_all_trays) (BroEngineAPI *self, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*rip) (BroEngineAPI *self, const char *drive_id, const BroRipOptionsDto *options, BroEngineJobDone done, gpointer data, GDestroyNotify destroy);
  void (*jobs) (BroEngineAPI *self, const BroJobQueryDto *query, BroEnginePageDone done, gpointer data, GDestroyNotify destroy);
  void (*job) (BroEngineAPI *self, const BroId *job_id, BroEngineDetailDone done, gpointer data, GDestroyNotify destroy);
  void (*job_log) (BroEngineAPI *self, const BroId *job_id, gint64 after, BroEngineListDone done, gpointer data, GDestroyNotify destroy);
  void (*cancel) (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*retry) (BroEngineAPI *self, const BroId *job_id, const BroStepKind *from_step, BroEngineJobDone done, gpointer data, GDestroyNotify destroy);
  void (*keep_apart) (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*decide) (BroEngineAPI *self, const BroId *job_id, const char *decision_id, const char *answer, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*move) (BroEngineAPI *self, const BroId *job_id, int by, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*start_now) (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*libraries) (BroEngineAPI *self, BroEngineListDone done, gpointer data, GDestroyNotify destroy);
  void (*attach_library) (BroEngineAPI *self, const char *path, const char *name, BroEngineLibraryDone done, gpointer data, GDestroyNotify destroy);
  void (*config) (BroEngineAPI *self, BroEngineEnvelopeDone done, gpointer data, GDestroyNotify destroy);
  void (*update_config) (BroEngineAPI *self, const BroJsonValue *doc, gint64 version, BroEngineEnvelopeDone done, gpointer data, GDestroyNotify destroy);
  void (*validate_config) (BroEngineAPI *self, const BroJsonValue *doc, BroEngineListDone done, gpointer data, GDestroyNotify destroy);
  void (*set_secret) (BroEngineAPI *self, const char *name, const char *value, BroEngineDone done, gpointer data, GDestroyNotify destroy);
  void (*export_config) (BroEngineAPI *self, BroEngineValueDone done, gpointer data, GDestroyNotify destroy);
  void (*events) (BroEngineAPI *self, gboolean has_after, gint64 after, const BroId *job_log, gsize n_job_log, BroEngineFeedDone done, gpointer data, GDestroyNotify destroy);
};

/* Versions, MakeMKV, tools, problems, libraries, configuration version, last event seq. */
void bro_engine_api_system (BroEngineAPI *self, BroSystemStatusDone done, gpointer data, GDestroyNotify destroy);

/* The stop order: running jobs are left as a killed engine would leave them, for the next start's recovery.
 * server.stopping when stopping already. */
void bro_engine_api_stop (BroEngineAPI *self, BroEngineDone done, gpointer data, GDestroyNotify destroy);

/* DriveService's drives: BroDriveDto *. */
void bro_engine_api_drives (BroEngineAPI *self, BroEngineListDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_eject (BroEngineAPI *self, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_close_tray (BroEngineAPI *self, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_close_all_trays (BroEngineAPI *self, BroEngineDone done, gpointer data, GDestroyNotify destroy);

/* A manual video-disc job on the drive's disc: drive.notFound, drive.noDisc, drive.busy, disc.notVideo; titles:
 * session.notOpen until SessionService; an outputFolder that is no library's: library.notAttached. */
void bro_engine_api_rip (BroEngineAPI *self, const char *drive_id, const BroRipOptionsDto *options, BroEngineJobDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_jobs (BroEngineAPI *self, const BroJobQueryDto *query, BroEnginePageDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_job (BroEngineAPI *self, const BroId *job_id, BroEngineDetailDone done, gpointer data, GDestroyNotify destroy);

/* BroLogLineDto *. */
void bro_engine_api_job_log (BroEngineAPI *self, const BroId *job_id, gint64 after, BroEngineListDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_cancel (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);

/* @from_step nullable. */
void bro_engine_api_retry (BroEngineAPI *self, const BroId *job_id, const BroStepKind *from_step, BroEngineJobDone done, gpointer data, GDestroyNotify destroy);

/* Nothing returned (204) once queued. */
void bro_engine_api_keep_apart (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_decide (BroEngineAPI *self, const BroId *job_id, const char *decision_id, const char *answer, BroEngineDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_move (BroEngineAPI *self, const BroId *job_id, int by, BroEngineDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_start_now (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy);

/* BroLibraryDto *. */
void bro_engine_api_libraries (BroEngineAPI *self, BroEngineListDone done, gpointer data, GDestroyNotify destroy);

/* Writes the folder's marker and adds the library to the configuration; if that save fails the marker is
 * removed again and the save's error returned. */
void bro_engine_api_attach_library (BroEngineAPI *self, const char *path, const char *name, BroEngineLibraryDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_config (BroEngineAPI *self, BroEngineEnvelopeDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_update_config (BroEngineAPI *self, const BroJsonValue *doc, gint64 version, BroEngineEnvelopeDone done, gpointer data, GDestroyNotify destroy);

/* BroIssue *. */
void bro_engine_api_validate_config (BroEngineAPI *self, const BroJsonValue *doc, BroEngineListDone done, gpointer data, GDestroyNotify destroy);

void bro_engine_api_set_secret (BroEngineAPI *self, const char *name, const char *value, BroEngineDone done, gpointer data, GDestroyNotify destroy);

/* The ExportBundle document. */
void bro_engine_api_export_config (BroEngineAPI *self, BroEngineValueDone done, gpointer data, GDestroyNotify destroy);

/* The event feed from @after (none: a snapshot first); jobLog events only for the jobs in @job_log. */
void bro_engine_api_events (BroEngineAPI *self, gboolean has_after, gint64 after, const BroId *job_log, gsize n_job_log, BroEngineFeedDone done, gpointer data, GDestroyNotify destroy);

G_END_DECLS
