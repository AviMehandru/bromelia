/* bro-config-service.h: BroConfigService: the configuration (plan §11.1, §15.1, §15.7, §24), used on the executor:
 * config.json (mode 0600), its version (kv config.version, so it survives a restart) and the last good document (kv
 * config.lastGood), and the secrets it refers to. The engine owns the secrets: clients set them through
 * bro_config_service_set_secret and only ever see whether one is set. A secret comes from the system keystore, then
 * BROMELIA_SECRET_<NAME>, then the secrets folder (plan §24); when two of them disagree, config secrets.prefer /
 * secrets.bySecret pick one, and the disagreement is a problem: secret.conflict (an error) without a preference that
 * settles it, secret.conflictPreferred (a warning) with one. A source that fails is the problem secret.unreadable until
 * it reads again. File, keystore and store work runs on workers. Implements BroConfigSource.
 * shared/fixtures/engine/config-service.cases.json. */
#pragma once

#include "bro-config-envelope.h"
#include "bro-config-source.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-file-system.h"
#include "bro-key-value-repository.h"
#include "bro-keystore.h"
#include "bro-problem-service.h"
#include "bro-secret-source.h"

G_BEGIN_DECLS

#define BRO_TYPE_CONFIG_SERVICE (bro_config_service_get_type ())
G_DECLARE_FINAL_TYPE (BroConfigService, bro_config_service, BRO, CONFIG_SERVICE, GObject)

/* Runs on the executor, never inside the call that started the work: @error is NULL when it succeeded. */
typedef void (*BroConfigDone) (const BroBroError *error, gpointer data);
/* The same with the envelope (yours to free; NULL on failure). */
typedef void (*BroEnvelopeDone) (BroConfigEnvelope *envelope, const BroBroError *error, gpointer data);

/* @keystore: where bro_config_service_set_secret stores secrets (the system store, or the secrets folder when there is
 * none). @secret_files: the secrets folder alone (the keystore adapter without its system store). @path: config.json.
 * @environment: the process's BROMELIA_SECRET_<NAME> variables (name → value, copied), read once
 * by the composition root (the engine never reads the environment itself). */
BroConfigService *bro_config_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroFileSystem *fs,
                                          BroKeystore *keystore, BroKeystore *secret_files, BroKeyValueRepository *kv, const char *path,
                                          GHashTable *environment);

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
 * config.saveFailed{reason} and leaves the configuration in use as it was. Publishes configChanged{version}, then
 * checks the secrets again (the preference may have changed). */
void bro_config_service_update (BroConfigService *self, const BroJsonValue *doc, gint64 expected_version, BroEnvelopeDone done,
                                gpointer data, GDestroyNotify destroy);

/* The version, the document, its issues and every secret it refers to (in document order) with whether it is set: in
 * any of its sources; a source that fails counts as not having it. */
void bro_config_service_envelope (BroConfigService *self, BroEnvelopeDone done, gpointer data, GDestroyNotify destroy);

/* A secret's value (g_free it), from the first source that has it: the system keystore, BROMELIA_SECRET_<NAME> (upper
 * case, '.' → '_'), the secrets folder; a source @config prefers (secrets.bySecret, else secrets.prefer) comes first
 * when it has one. NULL with no error when none has it; the first failure when a source failed and none has it
 * (fs.notPrivate on a share). Sources that disagree raise (or update) the secret's problem on the executor. Any thread
 * but the executor: the keystore may block. */
char *bro_config_service_secret (BroConfigService *self, const char *name, const BroConfig *config, BroBroError **error);

/* Stores a secret in the keystore ("" removes it), publishes configChanged{version} (the version doesn't change) and
 * checks the secrets again. The keystore's failure is returned. */
void bro_config_service_set_secret (BroConfigService *self, const char *name, const char *value, BroConfigDone done, gpointer data,
                                    GDestroyNotify destroy);

/* The export bundle: the configuration with its secret references, never their values. */
GBytes *bro_config_service_export (BroConfigService *self);

G_END_DECLS
