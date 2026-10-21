/* bro-seal-record.h: BroSealRecord, the archive record SealStep writes (bromelia-<unit8>.json,
 * shared/schema/archive-record-3.json), from what the job's steps found. Messages are kept as codes with parameters and
 * as text in the configured language. */
#pragma once

#include "bro-acquire-output.h"
#include "bro-job-snapshot.h"
#include "bro-message-renderer.h"
#include "bro-plan-output.h"

G_BEGIN_DECLS

/* A version 3 record's document (yours to unref). status: errors when MakeMKV reported read errors, else success.
 * disc: the listing Probe read and the identity Plan resolved. acquisition: the mode, "Backup" for a backup else "Rip",
 * the job's source as the request gives it (drive:<id>, iso:<path>, folder:<path>, file:<path>), the drive, the
 * profile, MakeMKV's version line and the app's. titles: each title Acquire ripped that the listing has, with its read
 * errors. files: @files (BroUnitFile *), as SHA256SUMS lists them. attempts: this job's. counts: the warnings and
 * errors in the job's log (@log: BroLogLineDto *). problems: each read error (makemkv.message), then rip.readErrors.
 * logs: the log files copied into the folder (@logs: char *). */
BroJsonValue *bro_seal_record_build (const BroJobSnapshot *job, const BroPlanOutput *plan, const BroAcquireOutput *acquired, const BroId *unit_id,
                                     GPtrArray *files, GPtrArray *log, GPtrArray *logs, BroInstant finished_at, const char *app_version,
                                     BroMessageRenderer *messages);

G_END_DECLS
