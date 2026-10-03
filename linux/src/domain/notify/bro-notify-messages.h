/* bro-notify-messages.h: BroNotifyMessages, the text of notifications, as message codes. */
#pragma once

#include "bro-folder-check.h"
#include "bro-job-summary.h"
#include "bro-outcome.h"
#include "bro-title-and-body.h"

G_BEGIN_DECLS

/* "MKV finished: Inception", then what happened to the files: saved (also when a post-processing step failed), kept
 * with read errors, or the job's error (else the outcome). */
BroTitleAndBody *bro_notify_messages_for_job (const BroJobSummary *job, BroOutcome outcome);

/* "Archive check: all 12 folder(s) OK", or how many are damaged with a line for each of the first ten (a folder that
 * isn't OK counts as damaged). */
BroTitleAndBody *bro_notify_messages_for_check (const BroFolderCheck *results, gsize n_results);

G_END_DECLS
