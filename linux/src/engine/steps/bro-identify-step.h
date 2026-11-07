/* bro-identify-step.h: BroIdentifyStep: Step 5 (plan §20.1), its phase 3 form: bro_identity_resolve from the listing
 * Probe read (or none), the disc's label, the drive's flags and the name and kind chosen in the session; a backup that
 * stays encrypted (mode backup before Plan) gets the e format code. Online lookups, the archive's previous disc and
 * menu numbers join in phase 5. Logs identity.resolved. Data: the identity. */
#pragma once

#include "bro-identity.h"
#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_IDENTIFY_STEP (bro_identify_step_get_type ())
G_DECLARE_FINAL_TYPE (BroIdentifyStep, bro_identify_step, BRO, IDENTIFY_STEP, GObject)

BroIdentifyStep *bro_identify_step_new (void);

/* The identity of the job's disc from Probe's listing (or none), the disc's label (the listing's name, else
 * bro_step_sources_label), the drive's flags and the session's name and kind; @encrypted: a backup that stays
 * encrypted. Plan resolves it again under the mode it chose. NULL and @error when the job names no source. */
BroIdentity *bro_identify_step_resolve (const BroStepContext *context, gboolean encrypted, BroBroError **error);

G_END_DECLS
