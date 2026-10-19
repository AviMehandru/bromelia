/* bro-acquired-title.h: BroAcquiredTitle, a title AcquireStep ripped: its index as the plan numbers it (the listing Probe
 * read), the files it made in the job's staging folder (MakeMKV's names), and what MakeMKV said about read errors while
 * ripping it. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  int index;
  GPtrArray *files;       /* char * */
  GPtrArray *read_errors; /* char * */
} BroAcquiredTitle;

/* No files, no read errors. */
BroAcquiredTitle *bro_acquired_title_new (int index);
BroAcquiredTitle *bro_acquired_title_copy (const BroAcquiredTitle *title);
void bro_acquired_title_free (BroAcquiredTitle *title);

BroJsonValue *bro_acquired_title_encode (const BroAcquiredTitle *title);
BroAcquiredTitle *bro_acquired_title_decode (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroAcquiredTitle, bro_acquired_title_free)

G_END_DECLS
