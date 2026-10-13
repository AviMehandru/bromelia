/* bro-commit-result.h: BroCommitResult: what a commit did: the unit, its folder (absolute), its files as committed, and
 * whether they were added to an existing folder. */
#pragma once

#include "bro-id.h"
#include "bro-unit-file.h"

G_BEGIN_DECLS

typedef struct {
  BroId unit_id;
  char *destination;
  GPtrArray *files; /* BroUnitFile * */
  gboolean merged;
} BroCommitResult;

/* Takes @files. */
BroCommitResult *bro_commit_result_new (BroId unit_id, const char *destination, GPtrArray *files, gboolean merged);
void bro_commit_result_free (BroCommitResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCommitResult, bro_commit_result_free)

G_END_DECLS
