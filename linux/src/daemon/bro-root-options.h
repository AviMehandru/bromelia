/* bro-root-options.h: BroRootOptions: what the graph is built for. Phase 3 has no defaults (HostOptions gives them in
 * phase 4): the platform's data folder is today's app's, with the real config.json and secrets, so a test always passes
 * its own. The fields are borrowed for the call. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  const char *data_dir;     /* bromelia.db, jobs, secrets, logs (made when it isn't there) */
  const char *config_path;  /* config.json; NULL: <data>/config.json */
  const char *home;         /* the user's home: MakeMKV's folder (~/.MakeMKV) and ~ in paths */
  gboolean system_keystore; /* the Secret Service (FALSE in tests: the secrets folder only) */
  gboolean system_tools;    /* look for the tools where they are usually installed and on PATH (FALSE in tests: no tool is
                             * found, so nothing runs makemkvcon) */
  gboolean drives;          /* use the optical drives (FALSE in tests: no device monitor, no drive control, no MakeMKV drive
                             * scan, so nothing asks a drive anything; real hosts pass TRUE) */
  GHashTable *environment;  /* char * → char *, the process's (the host passes it): ConfigService gets its
                             * BROMELIA_SECRET_* entries, the tool locator its PATH; NULL: empty */
} BroRootOptions;

G_END_DECLS
