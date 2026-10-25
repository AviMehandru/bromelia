/* bro-commit-output.h: BroCommitOutput, what CommitStep did: the unit, its library, its folder relative to the
 * library's root as committed (a free name in _Quarantine when quarantined), whether it was merged into a folder that is
 * shared, and whether it was quarantined (plan §15.4: the unit's id in the job's output). */
#pragma once

#include "bro-id.h"
#include "bro-job-snapshot.h"

G_BEGIN_DECLS

typedef struct {
  BroId unit_id;
  char *library;
  char *path;
  gboolean merged;
  gboolean quarantined;
} BroCommitOutput;

/* Empty. */
BroCommitOutput *bro_commit_output_new (void);
void bro_commit_output_free (BroCommitOutput *output);

BroJsonValue *bro_commit_output_encode (const BroCommitOutput *output);
BroCommitOutput *bro_commit_output_decode (const BroJsonValue *json);
/* The job's Commit output; NULL when Commit hasn't run. */
BroCommitOutput *bro_commit_output_of (const BroJobSnapshot *job);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCommitOutput, bro_commit_output_free)

G_END_DECLS
