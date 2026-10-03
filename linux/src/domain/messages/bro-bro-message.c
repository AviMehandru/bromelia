/* bro-bro-message.c */
#include "bro-bro-message.h"

BroBroMessage *
bro_bro_message_new (BroMessageCode code, BroJsonValue *params, BroSeverity severity)
{
  BroBroMessage *m = g_new0 (BroBroMessage, 1);
  m->code = code;
  m->params = params ? params : bro_json_value_new_object ();
  m->severity = severity;
  return m;
}

BroBroMessage *
bro_bro_message_copy (const BroBroMessage *message)
{
  return message ? bro_bro_message_new (message->code, bro_json_value_ref (message->params), message->severity) : NULL;
}

void
bro_bro_message_free (BroBroMessage *message)
{
  if (!message)
    return;
  bro_json_value_unref (message->params);
  g_free (message);
}

BroBroError *
bro_bro_message_to_error (const BroBroMessage *message, BroBroError *cause)
{
  return bro_bro_error_new (bro_message_code_wire (message->code), bro_json_value_ref (message->params), cause);
}

BroJsonValue *
bro_bro_message_to_json (const BroBroMessage *message)
{
  BroJsonValue *v = bro_json_value_new_object ();
  bro_json_value_set (v, "code", bro_json_value_new_string (bro_message_code_wire (message->code)));
  if (bro_json_value_length (message->params) > 0)
    bro_json_value_set (v, "params", bro_json_value_ref (message->params));
  return v;
}
