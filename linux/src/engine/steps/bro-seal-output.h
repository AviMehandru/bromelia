/* bro-seal-output.h: BroSealOutput, what SealStep wrote into the job's staging folder, for Commit: the unit's id, the
 * record's status (success, or errors after read errors), the files SHA256SUMS lists (path relative to the unit's
 * folder, size and SHA-256 as read back from disk, role, title: unit_files rows), the record's file name, the logs
 * copied, the note (READ ERRORS.txt) and the quarantine tag Commit uses ("READ ERRORS"; NULL: a finished archive,
 * committed into the library). */
#pragma once

#include "bro-id.h"
#include "bro-job-snapshot.h"
#include "bro-unit-file.h"

G_BEGIN_DECLS

typedef struct {
  BroId unit_id;
  char *status;
  GPtrArray *files; /* BroUnitFile *; role "" when it has none */
  char *record;
  GPtrArray *logs; /* char * */
  char *note;      /* nullable */
  char *tag;       /* nullable */
} BroSealOutput;

/* Empty. */
BroSealOutput *bro_seal_output_new (void);
void bro_seal_output_free (BroSealOutput *output);

BroJsonValue *bro_seal_output_encode (const BroSealOutput *output);
BroSealOutput *bro_seal_output_decode (const BroJsonValue *json);
/* The job's Seal output; NULL when Seal hasn't run. */
BroSealOutput *bro_seal_output_of (const BroJobSnapshot *job);

/* {path, size, sha256, role?, title?}: a file as SealOutput and the archive record write it. */
BroJsonValue *_bro_seal_output_file (const BroUnitFile *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSealOutput, bro_seal_output_free)

G_END_DECLS
