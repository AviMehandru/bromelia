/* bro-config-codec.h: BroConfigCodec, configuration documents, version 3: decoding fills defaults outside profiles,
 * writes keys in schema order and reports (and drops) unknown keys; encoding is canonical JSON. */
#pragma once

#include "bro-bro-error.h"
#include "bro-config.h"

G_BEGIN_DECLS

/* A version 3 document; NULL and @error (config.rejected) when the bytes aren't a JSON object. Other problems are
 * issues (see bro_config_validator_validate). */
BroConfig *bro_config_codec_decode (const char *bytes, gssize length, BroBroError **error);

/* Two-space indented JSON with a final newline, keys in schema order. */
GBytes *bro_config_codec_encode (const BroConfig *config);

/* {format: bromelia-config, version: 3, config}: the configuration without secrets (SecretRefs are kept; their
 * values never are in the document). */
GBytes *bro_config_codec_export_bundle (const BroConfig *config);

G_END_DECLS
