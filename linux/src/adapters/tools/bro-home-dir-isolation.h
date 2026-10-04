/* bro-home-dir-isolation.h: BroHomeDirIsolation: SettingsIsolation for macOS and Linux (plan §10.3): each makemkvcon
 * run gets HOME set to the job's home folder, holding its own settings.conf (0600: it may hold the registration key)
 * and the generated profile. The files stay with the job. */
#pragma once

#include "bro-file-system.h"
#include "bro-home-layout.h"
#include "bro-settings-isolation.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_HOME_DIR_ISOLATION (bro_home_dir_isolation_get_type ())
G_DECLARE_FINAL_TYPE (BroHomeDirIsolation, bro_home_dir_isolation, BRO, HOME_DIR_ISOLATION, GObject)

/* Keeps a reference to @fs. */
BroHomeDirIsolation *bro_home_dir_isolation_new (BroFileSystem *fs, BroHomeLayout layout);

/* Writes <work>/<layout's folder>/settings.conf and <work>/profile.mmcp.xml; the lease sets HOME to <work>. NULL and
 * @error set when a file can't be written. */
BroIsolationLease *bro_home_dir_isolation_prepare (BroHomeDirIsolation *self, const BroMakemkvRunSettings *settings,
                                                   BroBroError **error);

G_END_DECLS
