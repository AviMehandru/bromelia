/* bro-platform-process-launcher.h: BroPlatformProcessLauncher: starts processes in their own process group
 * (g_spawn + setpgid) and watches them from a thread of their own: stalls, stop escalation (INT →) TERM → KILL
 * 5 s apart, abandonment 30 s after KILL, output read for 5 s after exit (longer only while lines wait for a reader that
 * is still taking them), a transcript of every line. Lines are cut at 64 KiB (they end with " [cut]") and at most
 * BRO_QUEUED_LINES, or BRO_QUEUED_BYTES of text, wait to be read: then the reading waits too, so the tool waits to write
 * (nothing is lost) and that wait doesn't count as silence.
 *
 * Signals go to the whole group. Once a stopped process has ended, whatever is left of its group is killed (a
 * background child of a shell ignores SIGINT). A process left running after a normal exit is left alone. */
#pragma once

#include "bro-command-line.h"
#include "bro-process-launcher.h"
#include <glib-object.h>

G_BEGIN_DECLS

/* How many lines, and how much of their text, may wait to be read before the reading waits: long lines for a reader
 * that has stopped can't hold more than about 16 MiB. */
#define BRO_QUEUED_LINES 10000
#define BRO_QUEUED_BYTES (16 * 1024 * 1024)

#define BRO_TYPE_PLATFORM_PROCESS_LAUNCHER (bro_platform_process_launcher_get_type ())
G_DECLARE_FINAL_TYPE (BroPlatformProcessLauncher, bro_platform_process_launcher, BRO, PLATFORM_PROCESS_LAUNCHER, GObject)

BroPlatformProcessLauncher *bro_platform_process_launcher_new (void);

/* Starts the process; NULL and @error set (process.couldNotStart) when it can't be started. Free with
 * g_object_unref; the process keeps running and being watched until it ends. */
BroRunningProcess *bro_platform_process_launcher_start (BroPlatformProcessLauncher *self, const BroProcessSpec *spec,
                                                        BroBroError **error);

/* The program and arguments actually run: the spec's interpreter first, else /bin/sh for a file without the execute
 * bit (a script saved without chmod +x). */
BroCommandLine *bro_platform_process_launcher_invocation (const BroProcessSpec *spec);

G_END_DECLS
