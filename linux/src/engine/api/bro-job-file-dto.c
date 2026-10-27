/* bro-job-file-dto.c */
#include "bro-job-file-dto.h"

BroJobFileDto *
bro_job_file_dto_new (const char *path)
{
  BroJobFileDto *f = g_new0 (BroJobFileDto, 1);
  f->path = g_strdup (path);
  return f;
}

void
bro_job_file_dto_free (BroJobFileDto *file)
{
  if (!file)
    return;
  g_free (file->path);
  g_free (file->sha256);
  g_free (file->role);
  g_free (file);
}
