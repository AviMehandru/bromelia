/* bro-check-result.h: the verdict of a unit's check (common.json's CheckResult) */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_CHECK_RESULT_OK,
  BRO_CHECK_RESULT_DAMAGED,
  BRO_CHECK_RESULT_ERROR,
  BRO_CHECK_RESULT_STOPPED,
} BroCheckResult;

/* The wire form ("ok"), as in shared/schema/common.json. */
const char *bro_check_result_to_wire (BroCheckResult value);
gboolean bro_check_result_from_wire (const char *text, BroCheckResult *out);

G_END_DECLS
