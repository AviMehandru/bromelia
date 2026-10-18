/* bro-job-log.h: BroJobLog: a job's log file, <data>/jobs/<id>/log.jsonl (plan §16): one JSON object per line, {t,
 * severity, code, params}, in the canonical encoding written on one line, so every platform writes the same bytes. */
#pragma once

#include "bro-file-system.h"
#include "bro-log-line-dto.h"
#include <glib.h>

G_BEGIN_DECLS

#define BRO_JOB_LOG_FILE_NAME "log.jsonl"

/* The lines (BroLogLineDto), each ended by a line break (their seq is left out). */
GBytes *bro_job_log_encode (GPtrArray *lines);

/* The lines (BroLogLineDto) of a log file, each with its line number as its seq (from 1). A line that isn't a log line
 * keeps its number and is left out; so is an unfinished last line (no line break), as the runner counts. */
GPtrArray *bro_job_log_decode (GBytes *bytes);

/* Adds the lines (BroLogLineDto) to the end of the job folder's log (the folder made when it isn't there), written
 * atomically, 0600. */
gboolean bro_job_log_append (BroFileSystem *fs, const char *job_directory, GPtrArray *lines, BroBroError **error);

G_END_DECLS
