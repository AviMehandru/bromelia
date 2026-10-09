/* bro-home-dir-isolation.h: BroHomeDirIsolation: SettingsIsolation for macOS and Linux (plan §10.3): each makemkvcon
 * run gets HOME set to the job's home folder, holding its own settings.conf (0600: it may hold the registration key)
 * and the generated profile. The files stay with the job, except the key: release removes the app_Key line from
 * settings.conf, whatever wrote it, so the key doesn't wait in the job's folder for retention. A run that never
 * released its lease (the engine crashed) leaves the key there; startup recovery removes it with
 * bro_home_dir_isolation_scrub_key. */
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

/* Removes the app_Key line from the settings.conf of a run's home (@work_directory), as release does: for the home of
 * a run the engine never released (it crashed). makemkv.keyNotRemoved when the file is there but can't be rewritten
 * (free with bro_bro_message_free); NULL otherwise. */
BroBroMessage *bro_home_dir_isolation_scrub_key (BroHomeDirIsolation *self, const char *work_directory);

G_END_DECLS
