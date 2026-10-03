/* bro-apprise-args.h: BroAppriseArgs, the apprise command's arguments (after apprise or python -m apprise). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* -t title -b body url. */
GStrv bro_apprise_args_build (const char *url, const char *title, const char *body);

G_END_DECLS
