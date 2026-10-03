/* bro-issue.c */
#include "bro-issue.h"

BroIssue *
bro_issue_new (const char *path, BroMessageCode code, BroJsonValue *params, BroSeverity severity)
{
  BroIssue *i = g_new0 (BroIssue, 1);
  i->path = g_strdup (path);
  i->code = code;
  i->params = params ? params : bro_json_value_new_object ();
  i->severity = severity;
  return i;
}

BroIssue *
bro_issue_copy (const BroIssue *issue)
{
  return issue ? bro_issue_new (issue->path, issue->code, bro_json_value_ref (issue->params), issue->severity) : NULL;
}

void
bro_issue_free (BroIssue *issue)
{
  if (!issue)
    return;
  g_free (issue->path);
  bro_json_value_unref (issue->params);
  g_free (issue);
}

BroJsonValue *
bro_issue_to_json (const BroIssue *issue)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "path", bro_json_value_new_string (issue->path));
  bro_json_value_set (o, "code", bro_json_value_new_string (bro_message_code_wire (issue->code)));
  if (bro_json_value_length (issue->params) > 0)
    bro_json_value_set (o, "params", bro_json_value_ref (issue->params));
  bro_json_value_set (o, "severity", bro_json_value_new_string (bro_severity_to_wire (issue->severity)));
  return o;
}
