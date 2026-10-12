/* bro-session-snapshot.h: an opened disc as it was when the user ripped it: the listing it was opened with and the
 * choices made on it (plan §15.3). ReconcileStep checks the disc is still that disc. */
#pragma once

#include "bro-listing.h"
#include "bro-session-choices.h"

G_BEGIN_DECLS

typedef struct {
  BroListing *listing;
  BroSessionChoices *choices;
} BroSessionSnapshot;

/* Takes ownership of both. */
BroSessionSnapshot *bro_session_snapshot_new (BroListing *listing, BroSessionChoices *choices);
BroSessionSnapshot *bro_session_snapshot_copy (const BroSessionSnapshot *snapshot);
void bro_session_snapshot_free (BroSessionSnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSessionSnapshot, bro_session_snapshot_free)

G_END_DECLS
