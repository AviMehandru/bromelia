/* bro-config-service.h: BroConfigService: the configuration (plan §11.1, §15.1, §15.7, §24), used on the executor:
 * config.json (mode 0600), its version (kv config.version, so it survives a restart) and the last good document (kv
 * config.lastGood), and the secrets it refers to. The engine owns the secrets (owner, 2026-10-12, review L3-6):
 * clients set them through bro_config_service_set_secret and only ever see whether one is set. File, keystore and
 * store work runs on workers. Implements BroConfigSource. shared/fixtures/engine/config-service.cases.json. */
#pragma once

#include "bro-config-envelope.h"
#include "bro-config-source.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-file-system.h"
#include "bro-key-value-repository.h"
#include "bro-keystore.h"
#include "bro-problem-service.h"

G_BEGIN_DECLS

#define BRO_TYPE_CONFIG_SERVICE (bro_config_service_get_type ())
G_DECLARE_FINAL_TYPE (BroConfigService, bro_config_service, BRO, CONFIG_SERVICE, GObject)

/* Runs on the executor, never inside the call that started the work: @error is NULL when it succeeded. */
typedef void (*BroConfigDone) (const BroBroError *error, gpointer data);
/* The same with the envelope (yours to free; NULL on failure). */
typedef void (*BroEnvelopeDone) (BroConfigEnvelope *envelope, const BroBroError *error, gpointer data);

/* @path: config.json. @environment: the process's BROMELIA_SECRET_<NAME> variables (name → value, copied), read once
 * by the composition root (the engine never reads the environment itself). */
BroConfigService *bro_config_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroFileSystem *fs,
                                          BroKeystore *keystore, BroKeyValueRepository *kv, const char *path, GHashTable *environment);

/* At startup. A missing file gives the defaults; a file with errors (or not JSON) is left alone, the last good document
 * is used (the defaults when there is none) and config.invalid{count} is raised. @destroy frees @data afterwards. */
void bro_config_service_load (BroConfigService *self, BroConfigDone done, gpointer data, GDestroyNotify destroy);

/* The configuration in use (on the executor): a copy, the caller frees it. */
BroConfig *bro_config_service_current (BroConfigService *self);

/* The saved revision clients send back with an update. */
gint64 bro_config_service_version (BroConfigService *self);

/* The document's issues (BroIssue *); nothing is saved. NULL and @error (config.rejected) when @doc isn't an object. */
GPtrArray *bro_config_service_validate (BroConfigService *self, const BroJsonValue *doc, BroBroError **error);

/* Saves a document: config.versionConflict{current} unless @expected_version is the current version (or while another
 * update saves); config.rejected{count (errors), issues} when it has errors. The version and the last good document
 * are stored before config.json, so a crash or a failed write can only make a client reload; a failure is
 * config.saveFailed{reason} and leaves the configuration in use as it was. Publishes configChanged{version}. */
void bro_config_service_update (BroConfigService *self, const BroJsonValue *doc, gint64 expected_version, BroEnvelopeDone done,
                                gpointer data, GDestroyNotify destroy);

/* The version, the document, its issues and every secret it refers to (in document order) with whether it is set: in
 * the keystore, or given as BROMELIA_SECRET_<NAME>; a keystore failure counts as not set. */
void bro_config_service_envelope (BroConfigService *self, BroEnvelopeDone done, gpointer data, GDestroyNotify destroy);

/* A secret's value (g_free it): the keystore's, else BROMELIA_SECRET_<NAME> (upper case, '.' → '_'), else NULL with
 * no error; the keystore's failure when it fails and the environment has nothing (fs.notPrivate on a share, L3-8).
 * Any thread but the executor: the keystore may block. */
char *bro_config_service_secret (BroConfigService *self, const char *name, BroBroError **error);

/* Stores a secret in the keystore ("" removes it) and publishes configChanged{version} (the version doesn't change).
 * The keystore's failure is returned. */
void bro_config_service_set_secret (BroConfigService *self, const char *name, const char *value, BroConfigDone done, gpointer data,
                                    GDestroyNotify destroy);

/* The export bundle: the configuration with its secret references, never their values. */
GBytes *bro_config_service_export (BroConfigService *self);

G_END_DECLS
