/* bro-job-query-dto.c */
#include "bro-job-query-dto.h"

BroJobQueryDto *
bro_job_query_dto_new (void)
{
  BroJobQueryDto *q = g_new0 (BroJobQueryDto, 1);
  q->states = g_array_new (FALSE, FALSE, sizeof (BroJobState));
  q->outcomes = g_array_new (FALSE, FALSE, sizeof (BroOutcome));
  q->limit = BRO_JOB_QUERY_DTO_DEFAULT_LIMIT;
  return q;
}

void
bro_job_query_dto_free (BroJobQueryDto *q)
{
  if (!q)
    return;
  g_array_unref (q->states);
  g_array_unref (q->outcomes);
  g_free (q->drive_id);
  g_free (q->cursor);
  g_free (q);
}
