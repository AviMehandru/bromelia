/* bro-job-query.c */
#include "bro-job-query.h"

BroJobQuery *
bro_job_query_new (void)
{
  BroJobQuery *x = g_new0 (BroJobQuery, 1);
  return x;
}

void
bro_job_query_free (BroJobQuery *x)
{
  if (!x)
    return;
  g_free (x);
}
