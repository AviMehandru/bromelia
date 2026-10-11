/* bro-log-line-dto.c */
#include "bro-log-line-dto.h"

BroLogLineDto *
bro_log_line_dto_new (gint64 seq, BroInstant t, BroSeverity severity, BroMessageCode code, BroJsonValue *params)
{
  BroLogLineDto *l = g_new0 (BroLogLineDto, 1);
  l->seq = seq;
  l->t = t;
  l->severity = severity;
  l->code = code;
  l->params = params ? bro_json_value_ref (params) : bro_json_value_new_object ();
  return l;
}

void
bro_log_line_dto_free (BroLogLineDto *line)
{
  if (!line)
    return;
  bro_json_value_unref (line->params);
  g_free (line);
}

BroJsonValue *
bro_log_line_dto_json (const BroLogLineDto *line)
{
  BroJsonValue *o = bro_json_value_new_object ();
  g_autofree char *t = bro_instant_format (line->t);
  bro_json_value_set (o, "seq", bro_json_value_new_integer (line->seq));
  bro_json_value_set (o, "t", bro_json_value_new_string (t));
  bro_json_value_set (o, "severity", bro_json_value_new_string (bro_severity_to_wire (line->severity)));
  bro_json_value_set (o, "code", bro_json_value_new_string (bro_message_code_wire (line->code)));
  if (bro_json_value_length (line->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (line->params));
  return o;
}
