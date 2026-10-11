/* bro-log-line-dto.h: a line of a job's log (dtos.json LogLine). jobs/<id>/log.jsonl holds the same objects without
 * their seq. */
#pragma once

#include "bro-instant.h"
#include "bro-json-value.h"
#include "bro-message-code.h"
#include "bro-severity.h"

G_BEGIN_DECLS

typedef struct {
  gint64 seq;
  BroInstant t;
  BroSeverity severity;
  BroMessageCode code;
  BroJsonValue *params; /* an object */
} BroLogLineDto;

/* Takes a reference to @params (NULL: none). */
BroLogLineDto *bro_log_line_dto_new (gint64 seq, BroInstant t, BroSeverity severity, BroMessageCode code, BroJsonValue *params);
void bro_log_line_dto_free (BroLogLineDto *line);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_log_line_dto_json (const BroLogLineDto *line);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLogLineDto, bro_log_line_dto_free)

G_END_DECLS
