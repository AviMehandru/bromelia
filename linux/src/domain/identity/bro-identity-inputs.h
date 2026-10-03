/* bro-identity-inputs.h: what bro_identity_resolve works from: the listing (when there is one), the disc's label,
 * whether a backup stays encrypted, the drive's flags, a format already known (a backup's structure), the user's
 * choices, and how many episodes the menu plays in one title. */
#pragma once

#include "bro-disc-flags.h"
#include "bro-disc-format.h"
#include "bro-listing.h"
#include "bro-media-kind.h"

G_BEGIN_DECLS

typedef struct {
  BroListing *listing;    /* nullable */
  char *disc_label;
  gboolean encrypted;
  gboolean has_flags;
  BroDiscFlags flags;
  gboolean has_format;
  BroDiscFormat format;
  char *name_override;
  gboolean has_kind_override;
  BroMediaKind kind_override;
  int play_all_episodes;
} BroIdentityInputs;

/* Takes a reference to @listing. */
BroIdentityInputs *bro_identity_inputs_new (BroListing *listing, const char *disc_label, gboolean encrypted);
void bro_identity_inputs_free (BroIdentityInputs *inputs);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroIdentityInputs, bro_identity_inputs_free)

G_END_DECLS
