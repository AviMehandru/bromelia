/* bro-notes.h: BroNotes, the INCOMPLETE / READ ERRORS note, written into a folder whose files aren't a finished
 * archive. */
#pragma once

#include "bro-bro-error.h"
#include "bro-bro-message.h"
#include "bro-id.h"
#include "bro-outcome.h"

G_BEGIN_DECLS

/* The note's lines (BroBroMessage *): the job and its outcome, a warning that the files are not a finished archive,
 * the error (nullable), the read errors MakeMKV reported (@read_errors: BroRobotMessage *), and the log files copied
 * into the folder (@logs: the job's log, then MakeMKV's, when it was copied). */
GPtrArray *bro_notes_render (const BroId *job_id, BroOutcome outcome, const BroBroError *error, GPtrArray *read_errors, const char *const *logs);

G_END_DECLS
