/* bro-rip-check-result.c */
#include "bro-rip-check-result.h"

BroRipCheckResult *
bro_rip_check_result_new (void)
{
  BroRipCheckResult *r = g_new0 (BroRipCheckResult, 1);
  r->problems = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  r->notes = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  return r;
}

void
bro_rip_check_result_free (BroRipCheckResult *result)
{
  if (!result)
    return;
  g_ptr_array_unref (result->problems);
  g_ptr_array_unref (result->notes);
  g_free (result);
}
