/* bro-job-detail-dto.h: BroJobDetailDto: a job with its steps, files and commands (dtos.json JobDetail: the Job's
 * members, then these). Children are left out until JobQuery can ask for a job's children (decision 171). */
#pragma once

#include "bro-job-dto.h"
#include "bro-job-file-dto.h"
#include "bro-job-step-dto.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroJobDto *job;
  GPtrArray *steps;    /* BroJobStepDto */
  GPtrArray *files;    /* BroJobFileDto */
  GPtrArray *commands; /* char * */
  gint64 log_bytes;
} BroJobDetailDto;

/* @job taken; no steps, files or commands. */
BroJobDetailDto *bro_job_detail_dto_new (BroJobDto *job);
void bro_job_detail_dto_free (BroJobDetailDto *detail);

/* The Job's members, then steps, files, commands and logBytes; optional members left out when absent. Free with
 * bro_json_value_unref. */
BroJsonValue *bro_job_detail_dto_json (const BroJobDetailDto *detail);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobDetailDto, bro_job_detail_dto_free)

G_END_DECLS
