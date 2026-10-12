/* bro-config-envelope.h: the configuration as clients get it (dtos.json ConfigEnvelope): the saved revision, the
 * document, its issues, and every secret it refers to with whether it is set. */
#pragma once

#include "bro-config.h"
#include "bro-secret-status.h"

G_BEGIN_DECLS

typedef struct {
  gint64 version;
  BroConfig *config;
  GPtrArray *issues;  /* BroIssue * */
  GPtrArray *secrets; /* BroSecretStatus * */
} BroConfigEnvelope;

/* Takes ownership of @config, @issues and @secrets. */
BroConfigEnvelope *bro_config_envelope_new (gint64 version, BroConfig *config, GPtrArray *issues, GPtrArray *secrets);
void bro_config_envelope_free (BroConfigEnvelope *envelope);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_config_envelope_json (const BroConfigEnvelope *envelope);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroConfigEnvelope, bro_config_envelope_free)

G_END_DECLS
