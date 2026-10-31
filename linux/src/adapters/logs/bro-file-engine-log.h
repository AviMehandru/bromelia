/* bro-file-engine-log.h: BroFileEngineLog: the real EngineLog (plan §16; shared/fixtures/adapters/engine-log.cases.json):
 * each line appended to <data>/logs/engine.log through the FileSystem port, the objects a job's log.jsonl holds ({t,
 * severity, code, params}, t this clock's time at the call, params left out when empty) in the canonical encoding on
 * one line. The logs folder is made when it isn't there; the file is 0600. A line that can't be written is dropped.
 * Rotation and the redaction of secret values come with bromeliad (phase 4). Thread-safe. */
#pragma once

#include "bro-clock.h"
#include "bro-engine-log.h"
#include "bro-file-system.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_FILE_ENGINE_LOG (bro_file_engine_log_get_type ())
G_DECLARE_FINAL_TYPE (BroFileEngineLog, bro_file_engine_log, BRO, FILE_ENGINE_LOG, GObject)

/* path: <data>/logs/engine.log. Takes references to fs and clock. */
BroFileEngineLog *bro_file_engine_log_new (BroFileSystem *fs, BroClock *clock, const char *path);

void bro_file_engine_log_log (BroFileEngineLog *self, const BroBroMessage *message);

G_END_DECLS
