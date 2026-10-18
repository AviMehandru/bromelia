/* bro-plan-step.h: BroPlanStep: Step 6 (plan §20.1): decides what the job makes and appends the rest of its steps.
 * The profile: bro_profile_resolver_resolve with the identity's facts (name, label, format code, kind), the drive entry
 * and whether the rip is automatic; a session's choices are its session layer (the chosen profile's fields, then the
 * chosen library). The mode: the request's (options.mode); MKV when titles were chosen by hand; else
 * bro_mode_chooser_mode by the disc's format. A mode that makes MKV files needs the listing (disc.noInformation) and
 * titles: the session's ticked titles (numbered as Reconcile found them; titles.noneChosenExist when none is on the
 * disc), else bro_title_rules_select (each title logged; its error, titles.manualRequired, titles.noneMatched), then
 * bro_one_pass_plan and the space they need (bro_space_estimate_for_plan). The library: the profile's, else
 * defaultLibrary, else the first (library.none; library.notFound); an offline one doesn't fail the job: Decide waits for
 * it, blocked, then checks the archive and the space. Archived before (not for infoOnly): BroArchiveCheck; skip ends
 * the job (disc.alreadyArchived), ask appends Decide. The space is reserved here when no Decide follows
 * (BroSpaceCheck). Appends [Decide?], Acquire, Release, VerifyRips (archive.verifyRips, not for infoOnly), Name, Seal,
 * Commit, Notify; the report's plan has the mode, profile and library. Data: BroPlanOutput. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLAN_STEP (bro_plan_step_get_type ())
G_DECLARE_FINAL_TYPE (BroPlanStep, bro_plan_step, BRO, PLAN_STEP, GObject)

BroPlanStep *bro_plan_step_new (void);

G_END_DECLS
