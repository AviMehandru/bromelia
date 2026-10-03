/* bro-issue.h: a problem with the configuration: where (a JSON path such as profiles[0].mode.default), what (an
 * issue code with its parameters) and how bad (warning: ignored; error: the document is rejected). */
#pragma once

#include "bro-json-value.h"
#include "bro-message-code.h"
#include "bro-severity.h"

G_BEGIN_DECLS

typedef struct {
  char *path;
  BroMessageCode code;
  BroJsonValue *params; /* an object, never NULL */
  BroSeverity severity;
} BroIssue;

/* Takes ownership of @params (NULL → an empty object). */
BroIssue *bro_issue_new (const char *path, BroMessageCode code, BroJsonValue *params, BroSeverity severity);
BroIssue *bro_issue_copy (const BroIssue *issue);
void bro_issue_free (BroIssue *issue);

/* {path, code, params?, severity}, as the API's Issue. */
BroJsonValue *bro_issue_to_json (const BroIssue *issue);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroIssue, bro_issue_free)

G_END_DECLS
