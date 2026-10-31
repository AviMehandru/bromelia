/* bro-composition-root.h: BroCompositionRoot: builds the real object graph (plan §17, decision 175): ports → adapters →
 * services → LocalEngine; nothing else constructs adapters or services. TestCompositionRoot (tests/) builds the same
 * graph over fakes. */
#pragma once

#include "bro-local-engine.h"
#include "bro-root-options.h"

G_BEGIN_DECLS

/* The app version: meson's project_version(). */
#define BRO_COMPOSITION_ROOT_APP_VERSION BRO_HOST_APP_VERSION

/* Opens and migrates the store, builds every adapter, service, the runner and LocalEngine. Nothing runs until
 * bro_local_engine_start. Stop runs the release on the executor thread, which closes the store; the store itself is
 * freed with the engine, so unref the engine only once stop's done has come (or if it was never started). NULL and
 * @error set when the data folder can't be made or the store can't be opened or migrated. */
BroLocalEngine *bro_composition_root_build (const BroRootOptions *options, BroBroError **error);

G_END_DECLS
