/* bro-job-page-dto.h: a page of jobs (dtos.json JobPage): newest first; next is the cursor of the next page, NULL on
 * the last; total counts every job the query matches. */
#pragma once

#include "bro-job-dto.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  GPtrArray *items; /* BroJobDto */
  char *next;       /* nullable */
  int total;
} BroJobPageDto;

/* No items, no next, total 0. */
BroJobPageDto *bro_job_page_dto_new (void);
void bro_job_page_dto_free (BroJobPageDto *page);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_job_page_dto_json (const BroJobPageDto *page);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobPageDto, bro_job_page_dto_free)

G_END_DECLS
