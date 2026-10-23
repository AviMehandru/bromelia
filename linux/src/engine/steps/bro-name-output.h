/* bro-name-output.h: BroNameOutput, what NameStep decided (session F fills it; Seal reads it): the unit's folder relative
 * to the library root (Layouts.paths, ConflictNamer), its name (a quarantined unit goes to _Quarantine/<name> [<tag>]),
 * whether the folder is shared (a media server layout: the commit merges item by item), every produced item by the name
 * Name gave it in staging, and the identity when Name corrected it (the format a backup showed; NULL: Plan's stands, and
 * the key is left out). Name renames inside staging, so staging is laid out as the unit's folder will be. */
#pragma once

#include "bro-acquire-output.h"
#include "bro-identity.h"
#include "bro-job-snapshot.h"
#include "bro-named-file.h"

G_BEGIN_DECLS

typedef struct {
  char *folder;
  char *name;
  gboolean merge;
  GPtrArray *files;      /* BroNamedFile * */
  BroIdentity *identity; /* nullable, owned */
} BroNameOutput;

/* Empty. */
BroNameOutput *bro_name_output_new (void);
void bro_name_output_free (BroNameOutput *output);

BroJsonValue *bro_name_output_encode (const BroNameOutput *output);
BroNameOutput *bro_name_output_decode (const BroJsonValue *json);
/* The job's Name output; NULL when Name hasn't run. */
BroNameOutput *bro_name_output_of (const BroJobSnapshot *job);
/* The produced items (BroNamedFile *) as Acquire left them, when no Name step ran: each title's files (role title), the
 * files no title owns (no role), the backup (role backup) and disc-info.json (role discInfo). */
GPtrArray *bro_name_output_from_acquire (const BroAcquireOutput *acquired);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroNameOutput, bro_name_output_free)

G_END_DECLS
