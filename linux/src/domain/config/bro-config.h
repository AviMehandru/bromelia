/* bro-config.h: a configuration, version 3 (shared/schema/config-3.json): the document as decoded (keys in schema
 * order, defaults filled outside profiles, unknown keys dropped) and the issues decoding found. Typed views of its
 * entries: bro_config_profiles, _drives, _steps, _rules. */
#pragma once

#include "bro-drive-entry.h"
#include "bro-issue.h"
#include "bro-profile.h"
#include "bro-rule.h"
#include "bro-step-definition.h"

G_BEGIN_DECLS

typedef struct {
  BroJsonValue *document;
  GPtrArray *issues; /* BroIssue * */
} BroConfig;

/* Takes ownership of @document and @issues. */
BroConfig *bro_config_new (BroJsonValue *document, GPtrArray *issues);
void bro_config_free (BroConfig *config);

GPtrArray *bro_config_profiles (const BroConfig *config); /* BroProfile * */
GPtrArray *bro_config_drives (const BroConfig *config);   /* BroDriveEntry * */
GPtrArray *bro_config_steps (const BroConfig *config);    /* BroStepDefinition * */
GPtrArray *bro_config_rules (const BroConfig *config);    /* BroRule * */

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroConfig, bro_config_free)

G_END_DECLS
