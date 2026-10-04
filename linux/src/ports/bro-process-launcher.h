/* bro-process-launcher.h: BroProcessLauncher: Starts processes, never through a shell. */
#pragma once

#include "bro-bro-error.h"
#include "bro-process-spec.h"
#include "bro-running-process.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_PROCESS_LAUNCHER (bro_process_launcher_get_type ())
G_DECLARE_INTERFACE (BroProcessLauncher, bro_process_launcher, BRO, PROCESS_LAUNCHER, GObject)

struct _BroProcessLauncherInterface {
  GTypeInterface parent_iface;

  BroRunningProcess *(*start) (BroProcessLauncher *self, const BroProcessSpec *spec, BroBroError **error);
};

/* Starts the process; fails when it can't be started. */
BroRunningProcess *bro_process_launcher_start (BroProcessLauncher *self, const BroProcessSpec *spec, BroBroError **error);

G_END_DECLS
