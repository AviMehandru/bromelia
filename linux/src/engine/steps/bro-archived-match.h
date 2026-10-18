/* bro-archived-match.h: a unit that holds this disc already: its id, its folder (absolute) and the day it was committed
 * (YYYY-MM-DD, UTC). */
#pragma once

#include "bro-id.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  BroId unit_id;
  char *folder;
  char *day;
} BroArchivedMatch;

BroArchivedMatch *bro_archived_match_new (const BroId *unit_id, const char *folder, const char *day);
BroArchivedMatch *bro_archived_match_copy (const BroArchivedMatch *match);
void bro_archived_match_free (BroArchivedMatch *match);

BroJsonValue *bro_archived_match_encode (const BroArchivedMatch *match);
/* NULL unless @json is an object. */
BroArchivedMatch *bro_archived_match_decode (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroArchivedMatch, bro_archived_match_free)

G_END_DECLS
