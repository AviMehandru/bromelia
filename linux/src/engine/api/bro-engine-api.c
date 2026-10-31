/* bro-engine-api.c */
#include "bro-engine-api.h"

G_DEFINE_INTERFACE (BroEngineAPI, bro_engine_api, G_TYPE_OBJECT)

static void
bro_engine_api_default_init (BroEngineAPIInterface *iface)
{
}

void
bro_engine_api_system (BroEngineAPI *self, BroSystemStatusDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->system_status (self, done, data, destroy);
}

void
bro_engine_api_stop (BroEngineAPI *self, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->stop (self, done, data, destroy);
}

void
bro_engine_api_drives (BroEngineAPI *self, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->drives (self, done, data, destroy);
}

void
bro_engine_api_eject (BroEngineAPI *self, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->eject (self, drive_id, done, data, destroy);
}

void
bro_engine_api_close_tray (BroEngineAPI *self, const char *drive_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->close_tray (self, drive_id, done, data, destroy);
}

void
bro_engine_api_close_all_trays (BroEngineAPI *self, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->close_all_trays (self, done, data, destroy);
}

void
bro_engine_api_rip (BroEngineAPI *self, const char *drive_id, const BroRipOptionsDto *options, BroEngineJobDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->rip (self, drive_id, options, done, data, destroy);
}

void
bro_engine_api_jobs (BroEngineAPI *self, const BroJobQueryDto *query, BroEnginePageDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->jobs (self, query, done, data, destroy);
}

void
bro_engine_api_job (BroEngineAPI *self, const BroId *job_id, BroEngineDetailDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->job (self, job_id, done, data, destroy);
}

void
bro_engine_api_job_log (BroEngineAPI *self, const BroId *job_id, gint64 after, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->job_log (self, job_id, after, done, data, destroy);
}

void
bro_engine_api_cancel (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->cancel (self, job_id, done, data, destroy);
}

void
bro_engine_api_retry (BroEngineAPI *self, const BroId *job_id, const BroStepKind *from_step, BroEngineJobDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->retry (self, job_id, from_step, done, data, destroy);
}

void
bro_engine_api_keep_apart (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->keep_apart (self, job_id, done, data, destroy);
}

void
bro_engine_api_decide (BroEngineAPI *self, const BroId *job_id, const char *decision_id, const char *answer, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->decide (self, job_id, decision_id, answer, done, data, destroy);
}

void
bro_engine_api_move (BroEngineAPI *self, const BroId *job_id, int by, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->move (self, job_id, by, done, data, destroy);
}

void
bro_engine_api_start_now (BroEngineAPI *self, const BroId *job_id, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->start_now (self, job_id, done, data, destroy);
}

void
bro_engine_api_libraries (BroEngineAPI *self, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->libraries (self, done, data, destroy);
}

void
bro_engine_api_attach_library (BroEngineAPI *self, const char *path, const char *name, BroEngineLibraryDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->attach_library (self, path, name, done, data, destroy);
}

void
bro_engine_api_config (BroEngineAPI *self, BroEngineEnvelopeDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->config (self, done, data, destroy);
}

void
bro_engine_api_update_config (BroEngineAPI *self, const BroJsonValue *doc, gint64 version, BroEngineEnvelopeDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->update_config (self, doc, version, done, data, destroy);
}

void
bro_engine_api_validate_config (BroEngineAPI *self, const BroJsonValue *doc, BroEngineListDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->validate_config (self, doc, done, data, destroy);
}

void
bro_engine_api_set_secret (BroEngineAPI *self, const char *name, const char *value, BroEngineDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->set_secret (self, name, value, done, data, destroy);
}

void
bro_engine_api_export_config (BroEngineAPI *self, BroEngineValueDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->export_config (self, done, data, destroy);
}

void
bro_engine_api_events (BroEngineAPI *self, gboolean has_after, gint64 after, const BroId *job_log, gsize n_job_log, BroEngineFeedDone done, gpointer data, GDestroyNotify destroy)
{
  g_return_if_fail (BRO_IS_ENGINE_API (self));
  BRO_ENGINE_API_GET_IFACE (self)->events (self, has_after, after, job_log, n_job_log, done, data, destroy);
}
