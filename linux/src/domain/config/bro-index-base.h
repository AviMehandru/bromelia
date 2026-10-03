/* bro-index-base.h: what an index pattern counts: MakeMKV's title numbers or the source (playlist / VTS) ids */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_INDEX_BASE_MAKEMKV,
  BRO_INDEX_BASE_SOURCE,
} BroIndexBase;

/* The wire form ("makemkv"), as in shared/schema/common.json. */
const char *bro_index_base_to_wire (BroIndexBase value);
gboolean bro_index_base_from_wire (const char *text, BroIndexBase *out);

G_END_DECLS
