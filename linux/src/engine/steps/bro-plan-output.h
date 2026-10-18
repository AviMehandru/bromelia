/* bro-plan-output.h: what PlanStep decided, for the steps after it: the mode, the profile (its id and every field, so a
 * resumed job keeps it), the library and whether it was offline, the titles to rip (numbered as in the listing Probe
 * read; with the tracks and file name chosen by hand), the minimum length of a one-pass rip, the bytes the titles need
 * (before the margin; none: no check) and whether they are reserved, the archived unit Decide asks about, and the
 * identity under the mode. */
#pragma once

#include "bro-archived-match.h"
#include "bro-identity.h"
#include "bro-job-snapshot.h"
#include "bro-rip-mode.h"
#include "bro-title-choice.h"

G_BEGIN_DECLS

typedef struct {
  BroRipMode mode;
  char *profile_id;
  BroJsonValue *profile;
  char *library_id;
  gboolean library_offline;
  GPtrArray *titles; /* BroTitleChoice *, selected */
  gboolean has_one_pass_min_length;
  int one_pass_min_length;
  gboolean has_space_need;
  gint64 space_need;
  gboolean space_reserved;
  BroArchivedMatch *ask_archived; /* nullable */
  BroIdentity *identity;
} BroPlanOutput;

/* Empty: mode mkv, no titles, no library, an empty identity. */
BroPlanOutput *bro_plan_output_new (void);
void bro_plan_output_free (BroPlanOutput *plan);

BroJsonValue *bro_plan_output_encode (const BroPlanOutput *plan);
BroPlanOutput *bro_plan_output_decode (const BroJsonValue *json);
/* The plan in the job's Plan output; NULL when Plan hasn't run. */
BroPlanOutput *bro_plan_output_of (const BroJobSnapshot *job);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroPlanOutput, bro_plan_output_free)

G_END_DECLS
