/* bro-job-query-dto.h: which jobs a client asks for (dtos.json JobQuery, the query string of GET /v1/jobs): any of the
 * states, any of the outcomes (empty: any), a kind, a queue, a drive, a unit; a page of at most limit (1-500) from
 * cursor (the last page's next). */
#pragma once

#include "bro-id.h"
#include "bro-job-kind.h"
#include "bro-job-state.h"
#include "bro-outcome.h"
#include "bro-queue.h"
#include <glib.h>

G_BEGIN_DECLS

#define BRO_JOB_QUERY_DTO_DEFAULT_LIMIT 50
#define BRO_JOB_QUERY_DTO_MAX_LIMIT 500

typedef struct {
  GArray *states;   /* BroJobState */
  GArray *outcomes; /* BroOutcome */
  gboolean has_kind;
  BroJobKind kind;
  gboolean has_queue;
  BroQueue queue;
  char *drive_id; /* nullable */
  gboolean has_unit_id;
  BroId unit_id;
  char *cursor; /* nullable */
  int limit;
} BroJobQueryDto;

/* The lists empty, nothing else asked, limit BRO_JOB_QUERY_DTO_DEFAULT_LIMIT. */
BroJobQueryDto *bro_job_query_dto_new (void);
void bro_job_query_dto_free (BroJobQueryDto *query);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobQueryDto, bro_job_query_dto_free)

G_END_DECLS
