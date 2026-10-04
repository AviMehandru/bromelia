/* bro-tool-info.h: where a tool is (none: not found), its version, what it can do, and why it was or wasn't found. */
#pragma once

#include "bro-bro-message.h"
#include "bro-tool-kind.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroToolKind tool;
  char *path; /* nullable */
  char *version; /* nullable */
  GStrv capabilities;
  BroBroMessage *why; /* nullable */
} BroToolInfo;

/* Empty lists, nothing else set. */
BroToolInfo *bro_tool_info_new (void);
void bro_tool_info_free (BroToolInfo *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroToolInfo, bro_tool_info_free)

G_END_DECLS
