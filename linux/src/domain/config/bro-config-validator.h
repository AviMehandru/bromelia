/* bro-config-validator.h: BroConfigValidator, every problem of a configuration, with its JSON path: the issues
 * decoding found (unknown keys), values the schema doesn't allow (config.invalidValue, config.required;
 * config.secretInline for a secret written into the document), regular expressions that don't compile, references
 * to entries that don't exist, ids used twice, and the server's rules (a network address needs a token; TLS needs
 * both files). */
#pragma once

#include "bro-config.h"

G_BEGIN_DECLS

/* BroIssue *. */
GPtrArray *bro_config_validator_validate (const BroConfig *config);

G_END_DECLS
