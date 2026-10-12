/* bro-config-source.h: BroConfigSource: where the runner gets the configuration a job runs with (implemented by
 * ConfigService). It is read once, on the executor, when the job starts, and every step of that run sees the same
 * snapshot (owner, 2026-10-11). */
#pragma once

#include "bro-config.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_CONFIG_SOURCE (bro_config_source_get_type ())
G_DECLARE_INTERFACE (BroConfigSource, bro_config_source, BRO, CONFIG_SOURCE, GObject)

struct _BroConfigSourceInterface {
  GTypeInterface parent_iface;
  BroConfig *(*current) (BroConfigSource *self);
};

/* The configuration now; the caller frees it. */
BroConfig *bro_config_source_current (BroConfigSource *self);

G_END_DECLS
