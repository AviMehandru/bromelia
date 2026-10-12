/* bro-makemkv-setup.h: BroMakemkvSetup: how a step gets what a makemkvcon run needs (implemented by MakemkvService,
 * which holds the registration key and the user's MakeMKV data folder): the settings, the profile file and the
 * switches of a profile, run from the job's folder. Thread-safe. */
#pragma once

#include "bro-bro-error.h"
#include "bro-effective-profile.h"
#include "bro-makemkv-invocation.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MAKEMKV_SETUP (bro_makemkv_setup_get_type ())
G_DECLARE_INTERFACE (BroMakemkvSetup, bro_makemkv_setup, BRO, MAKEMKV_SETUP, GObject)

struct _BroMakemkvSetupInterface {
  GTypeInterface parent_iface;
  BroMakemkvInvocation *(*invocation) (BroMakemkvSetup *self, const BroEffectiveProfile *profile, const char *selection_override,
                                       const char *job_directory, BroBroError **error);
};

/* @selection_override: app_DefaultSelectionString for this run (hand-picked tracks: +sel:all); NULL keeps the
 * profile's. The invocation's option strings live as long as @self. */
BroMakemkvInvocation *bro_makemkv_setup_invocation (BroMakemkvSetup *self, const BroEffectiveProfile *profile, const char *selection_override,
                                                    const char *job_directory, BroBroError **error);

G_END_DECLS
