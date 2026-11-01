/* bro-scenario-catalog.h: the shared scenarios this runner plays (decisions 233, 234): the JSON copies in
 * shared/scenarios/json, next to shared/fixtures ($BROMELIA_FIXTURES/../scenarios/json), read as BroJsonValue. A runner
 * selects every scenario with steps whose phase it has built (BRO_SCENARIO_BUILT_PHASE) and whose platforms, when given,
 * name its own; realDisc scenarios are session I's. Test code, not shipped. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

/* The engine phase this runner can run (each phase raises it on all three platforms). */
#define BRO_SCENARIO_BUILT_PHASE 3
/* The platform name a scenario's platforms list uses for this runner. */
#define BRO_SCENARIO_PLATFORM "linux"

/* shared/scenarios/json (free with g_free). */
char *bro_scenario_catalog_folder (void);
/* The ids of the scenarios this runner plays, sorted (char *; free the array). */
GPtrArray *bro_scenario_catalog_selected (void);
/* Whether this runner plays the scenario. */
gboolean bro_scenario_catalog_selects (const BroJsonValue *scenario);
/* A scenario by id (NULL when there is none). */
BroJsonValue *bro_scenario_catalog_load (const char *id);
/* A file under shared/scenarios/json; a path the scenario gives (tools/x.yaml) is read as its JSON copy. */
BroJsonValue *bro_scenario_catalog_read (const char *relative);
/* A disc: a name in discs/, or a definition written inline (a reference either way). */
BroJsonValue *bro_scenario_catalog_disc (BroJsonValue *disc);
/* A tool script: a path (tools/x.yaml), or a script written inline (a reference either way). */
BroJsonValue *bro_scenario_catalog_script (BroJsonValue *script);
/* The named robot lines (tools/lines.yaml): name → text (free with g_hash_table_unref). */
GHashTable *bro_scenario_catalog_lines (void);

G_END_DECLS
