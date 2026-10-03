/* bro-rip-check-result.h: problems (the file must not be trusted: a wrong or truncated title) and notes
 * (differences worth logging that don't make the file unusable) of bro_rip_check_check. */
#pragma once

#include "bro-bro-message.h"

G_BEGIN_DECLS

typedef struct {
  GPtrArray *problems; /* BroBroMessage * */
  GPtrArray *notes;    /* BroBroMessage * */
} BroRipCheckResult;

/* Both lists empty. */
BroRipCheckResult *bro_rip_check_result_new (void);
void bro_rip_check_result_free (BroRipCheckResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRipCheckResult, bro_rip_check_result_free)

G_END_DECLS
