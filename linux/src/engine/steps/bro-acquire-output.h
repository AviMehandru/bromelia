/* bro-acquire-output.h: BroAcquireOutput, what AcquireStep left in the job's staging folder, by name (plan §12.4; review
 * L3-1: the steps after it use these lists, never a count): the titles in plan order, the files of a run no title owns (a
 * file an "all" run made that the listing doesn't name, and what a failed run left), the backup (a folder or an image)
 * when it is kept, disc-info.json when it was written, and the read errors of runs that ripped several titles at once or
 * made the backup. */
#pragma once

#include "bro-acquired-title.h"
#include "bro-job-snapshot.h"

G_BEGIN_DECLS

typedef struct {
  GPtrArray *titles;      /* BroAcquiredTitle * */
  GPtrArray *others;      /* char * */
  char *backup;           /* nullable */
  char *disc_info;        /* nullable */
  GPtrArray *read_errors; /* char * */
} BroAcquireOutput;

/* Empty. */
BroAcquireOutput *bro_acquire_output_new (void);
void bro_acquire_output_free (BroAcquireOutput *output);

BroJsonValue *bro_acquire_output_encode (const BroAcquireOutput *output);
BroAcquireOutput *bro_acquire_output_decode (const BroJsonValue *json);
/* The job's Acquire output; NULL when Acquire hasn't run. */
BroAcquireOutput *bro_acquire_output_of (const BroJobSnapshot *job);
/* Every name (char *) Acquire made in staging: the titles' files, the others, the backup and disc-info.json. */
GPtrArray *bro_acquire_output_produced (const BroAcquireOutput *output);
/* How many read errors MakeMKV reported in all. */
int bro_acquire_output_read_error_count (const BroAcquireOutput *output);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroAcquireOutput, bro_acquire_output_free)

G_END_DECLS
