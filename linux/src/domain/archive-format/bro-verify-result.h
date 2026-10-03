/* bro-verify-result.h: a unit's check: the verdict, the number of files SHA256SUMS lists, the files that changed,
 * couldn't be read or are missing, the files the folder has that SHA256SUMS doesn't list (not an error:
 * post-processing may add files), and the summary (check.ok / check.damaged, or the error: check.sumsEmpty). */
#pragma once

#include "bro-bro-message.h"
#include "bro-check-result.h"
#include "bro-file-verdict.h"

G_BEGIN_DECLS

typedef struct {
  BroCheckResult result;
  int files;
  GStrv changed;
  GStrv unreadable;
  GStrv missing;
  GStrv unlisted;
  BroBroMessage *summary;
} BroVerifyResult;

/* From the paths SHA256SUMS lists (@sums), what hashing each found (@hashes: path → GINT_TO_POINTER
 * (BroFileVerdict); a listed path without a verdict counts as missing), and the folder's files (relative paths,
 * without nested archives: the adapter stops at folders with a SHA256SUMS of their own). Bromelia's own files at
 * the top of the folder, media-server metadata and hidden files aren't unlisted. */
BroVerifyResult *bro_verify_result_compare (const char *const *sums, GHashTable *hashes, const char *const *folder_files);
void bro_verify_result_free (BroVerifyResult *result);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroVerifyResult, bro_verify_result_free)

G_END_DECLS
