/* bro-notify-step.h: BroNotifyStep: Step 18 (plan §20.1), a placeholder in Phase 3: it stays in Plan's list and only
 * reports success; the runner publishes JobFinished. The outbox and the sending come with NotificationService in Phase
 * 5. Data: {}. */
#pragma once

#include "bro-step.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_NOTIFY_STEP (bro_notify_step_get_type ())
G_DECLARE_FINAL_TYPE (BroNotifyStep, bro_notify_step, BRO, NOTIFY_STEP, GObject)

BroNotifyStep *bro_notify_step_new (void);

G_END_DECLS
