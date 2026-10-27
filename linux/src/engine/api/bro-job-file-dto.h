/* bro-job-file-dto.h: BroJobFileDto: a file a job made, as JobDetail lists it (dtos.json JobDetail.files): Seal's (path
 * in the unit folder, size and SHA-256 as read back from disk, role), else Acquire's (path in staging only). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *path;
  gboolean has_size;
  gint64 size;
  char *sha256; /* nullable */
  char *role;   /* nullable */
} BroJobFileDto;

/* @path copied; no size, SHA-256 or role. */
BroJobFileDto *bro_job_file_dto_new (const char *path);
void bro_job_file_dto_free (BroJobFileDto *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJobFileDto, bro_job_file_dto_free)

G_END_DECLS
