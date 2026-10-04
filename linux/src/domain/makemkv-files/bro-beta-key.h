/* bro-beta-key.h: BroBetaKey, MakeMKV's free beta key, replaced about once a month. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Whether an automatic update may replace @current_key (nullable): only a beta key (T-…) or no key, never a
 * purchased one. */
gboolean bro_beta_key_may_replace (const char *current_key);

G_END_DECLS
