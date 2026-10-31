/* bro-system-status-dto.h: BroSystemStatusDto: the engine as clients see it (dtos.json SystemStatus): the facts the
 * host gave, when start ran, whether MakeMKV is found and its version, the tools phase 3 runs as the ToolLocator finds
 * them (makemkvcon, mkvmerge), the problems, the libraries, the configuration's version and the last event's seq.
 * MakeMKV's registration is unknown until MakemkvService's phase 5 part. */
#pragma once

#include "bro-engine-facts.h"
#include "bro-instant.h"
#include "bro-json-value.h"
#include "bro-tool-info.h"

G_BEGIN_DECLS

typedef struct {
  BroEngineFacts *facts;
  BroInstant started_at;
  gboolean makemkv_found;
  char *makemkv_version; /* nullable */
  GPtrArray *tools;      /* BroToolInfo * */
  GPtrArray *problems;   /* BroProblemDto * */
  GPtrArray *libraries;  /* BroLibraryDto * */
  gint64 config_version;
  gint64 event_seq;
} BroSystemStatusDto;

/* Empty lists, nothing else set. */
BroSystemStatusDto *bro_system_status_dto_new (void);
void bro_system_status_dto_free (BroSystemStatusDto *status);

/* Optional members left out when absent; a tool is found when it has a path, its why carries its severity. Free with
 * bro_json_value_unref. */
BroJsonValue *bro_system_status_dto_json (const BroSystemStatusDto *status);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSystemStatusDto, bro_system_status_dto_free)

G_END_DECLS
