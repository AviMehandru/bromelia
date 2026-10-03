/* bro-bro-message.h: what logs, progress, problems and notifications carry (plan §16): a code, its parameters
 * and a severity. The engine never builds sentences; clients render the code from shared/messages. */
#pragma once

#include "bro-bro-error.h"
#include "bro-json-value.h"
#include "bro-message-code.h"
#include "bro-severity.h"

G_BEGIN_DECLS

typedef struct {
  BroMessageCode code;
  BroJsonValue *params; /* an object, never NULL */
  BroSeverity severity;
} BroBroMessage;

/* Takes ownership of @params (NULL → an empty object). */
BroBroMessage *bro_bro_message_new (BroMessageCode code, BroJsonValue *params, BroSeverity severity);
BroBroMessage *bro_bro_message_copy (const BroBroMessage *message);
void bro_bro_message_free (BroBroMessage *message);

/* The message as an expected failure; takes ownership of @cause. */
BroBroError *bro_bro_message_to_error (const BroBroMessage *message, BroBroError *cause);

/* {"code": …, "params": {…}}, as in shared/schema/common.json (params left out when empty). */
BroJsonValue *bro_bro_message_to_json (const BroBroMessage *message);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroBroMessage, bro_bro_message_free)

G_END_DECLS
