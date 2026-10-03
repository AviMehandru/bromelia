/* bro-config-private.h: the compiled configuration schema and the walker over it (internal in Swift and C# too). */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

/* tools/gen-config-schema.py's text: config-3.json with common.json's $defs merged in. */
const char *_bro_config_schema_text (void);

/* The parsed schema (borrowed; parsed once). */
BroJsonValue *_bro_schema_document (void);
BroJsonValue *_bro_schema_def (const char *name);

/* The value with the schema's keys in order, unknown keys dropped (each reported as a BroIssue in @issues, in
 * document order), and, unless @fill is FALSE or the node is sparse (x-inherit), missing keys filled with their
 * defaults. The same algorithm as tools/check-contracts.py's expand_defaults. */
BroJsonValue *_bro_schema_normalize (BroJsonValue *value, BroJsonValue *node, const char *path, gboolean fill, GPtrArray *issues);

/* The issues (BroIssue *) of a value against a node: type, const, enum, required, minimum / maximum, pattern,
 * minProperties, oneOf / anyOf, allOf, if / then and not; config.secretInline for text where a SecretRef goes. */
void _bro_schema_validate (BroJsonValue *value, BroJsonValue *node, const char *path, GPtrArray *issues);

G_END_DECLS
