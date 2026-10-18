/* bro-job-query.c */
#include "bro-job-query.h"

BroJobQuery *
bro_job_query_new (void)
{
  BroJobQuery *x = g_new0 (BroJobQuery, 1);
  x->states = g_array_new (FALSE, FALSE, sizeof (BroJobState));
  x->outcomes = g_array_new (FALSE, FALSE, sizeof (BroOutcome));
  return x;
}

void
bro_job_query_free (BroJobQuery *x)
{
  if (!x)
    return;
  g_array_unref (x->states);
  g_array_unref (x->outcomes);
  g_free (x->drive_id);
  g_free (x);
}
