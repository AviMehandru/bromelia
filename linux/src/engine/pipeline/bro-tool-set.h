/* bro-tool-set.h: the tools a step may run, as ports. More join with their steps in phase 5. */
#pragma once

#include "bro-makemkv.h"
#include "bro-matroska.h"

G_BEGIN_DECLS

typedef struct {
  BroMakemkv *makemkv;   /* a reference; NULL in tests that don't run it */
  BroMatroska *matroska; /* a reference; nullable likewise */
} BroToolSet;

/* Takes references. */
BroToolSet *bro_tool_set_new (BroMakemkv *makemkv, BroMatroska *matroska);
BroToolSet *bro_tool_set_copy (const BroToolSet *tools);
void bro_tool_set_free (BroToolSet *tools);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroToolSet, bro_tool_set_free)

G_END_DECLS
