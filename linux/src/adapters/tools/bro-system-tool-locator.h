/* bro-system-tool-locator.h: BroSystemToolLocator: ToolLocator (plan §9; shared/fixtures/adapters/tool-locator.cases.json):
 * the configured path (when set, nothing else is tried), else mkvextract next to the mkvmerge that was found, else the
 * candidates in order, else each folder of the search path with each program name in order. Versions and
 * capabilities come from the tools themselves. */
#pragma once

#include "bro-file-system.h"
#include "bro-tool-locator.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SYSTEM_TOOL_LOCATOR (bro_system_tool_locator_get_type ())
G_DECLARE_FINAL_TYPE (BroSystemToolLocator, bro_system_tool_locator, BRO, SYSTEM_TOOL_LOCATOR, GObject)

/* @configured: BroToolKind (GINT_TO_POINTER) → char * (empty: unset; the paths of tools.* and makemkv.path).
 * @candidates and @names: BroToolKind → GStrv (PlatformToolPaths); a tool without names uses its wire name.
 * @search_path: the folders of PATH. "~/" stands for @home. Keeps references to the tables. */
BroSystemToolLocator *bro_system_tool_locator_new (BroFileSystem *fs, GHashTable *configured, GHashTable *candidates, GHashTable *names,
                                                   const char *const *search_path, const char *home);

BroToolInfo *bro_system_tool_locator_locate (BroSystemToolLocator *self, BroToolKind tool);

G_END_DECLS
