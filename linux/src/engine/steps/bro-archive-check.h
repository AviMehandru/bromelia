/* bro-archive-check.h: BroArchiveCheck, whether the disc was archived before (plan §20.1 Plan and Decide). */
#pragma once

#include "bro-archived-match.h"
#include "bro-message-code.h"
#include "bro-step-context.h"

G_BEGIN_DECLS

/* The newest committed unit with this fingerprint that still counts: its status is success, its library is online and
 * its folder is there (read-error units, failures and vanished folders don't count). NULL without a fingerprint or a
 * match; NULL and @error set when the catalogue can't be read. */
BroArchivedMatch *bro_archive_check_find (const BroStepContext *context, const char *fingerprint, BroBroError **error);

/* What the job does about it. A rip started by hand, or automation.alreadyArchived ripAgain: logs disc.archivedBefore
 * and goes on (NULL). skip: the reason to skip the job, disc.alreadyArchived. ask: NULL (Decide asks:
 * bro_archive_check_asks). @config_id nullable. */
BroBroMessage *bro_archive_check_apply (const BroStepContext *context, const BroArchivedMatch *match, const char *config_id);

/* Whether the job asks about a disc archived before: an automatic rip under automation.alreadyArchived ask. */
gboolean bro_archive_check_asks (const BroStepContext *context, const char *config_id);

/* The message about the unit: {folder, day}, and its unitId for a reason to skip; a warning. */
BroBroMessage *bro_archive_check_message (BroMessageCode code, const BroArchivedMatch *match, gboolean with_unit);

G_END_DECLS
