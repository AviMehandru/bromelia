/* bro-plan-step.h: BroPlanStep: Step 6 (plan §20.1): decides what the job makes and appends the rest of its steps: the
 * profile (ProfileResolver.resolve; a session's choices, else the request's options), the mode (the request's; MKV when
 * titles were chosen by hand; else ModeChooser by the disc's format), the titles (the session's ticked ones, numbered
 * as Reconcile found them, else TitleRules.select), the library and the space.
 *
 * A mode that makes MKV files needs the listing and titles. An offline library doesn't fail the job: Decide waits for
 * it, blocked, then checks the archive and the space. A disc archived before ends the job under skip and appends Decide
 * under ask; the space is reserved here only when no Decide follows.
 *
 * Appends [Decide?], Acquire, Release, VerifyRips (archive.verifyRips, not for infoOnly), Name, Seal, Commit, Notify.
 * Data: PlanOutput. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PLAN_STEP (bro_plan_step_get_type ())
G_DECLARE_FINAL_TYPE (BroPlanStep, bro_plan_step, BRO, PLAN_STEP, GObject)

BroPlanStep *bro_plan_step_new (void);

G_END_DECLS
