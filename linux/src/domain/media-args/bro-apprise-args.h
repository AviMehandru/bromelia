/* bro-apprise-args.h: BroAppriseArgs, the apprise command's arguments (after apprise or python -m apprise) and
 * environment. The URL goes in APPRISE_URLS, never on the command line: it holds tokens (Telegram, Pushover), and a
 * command line is visible to every user of the machine (ps, /proc/<pid>/cmdline, a Docker host). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* -t title -b body. */
GStrv bro_apprise_args_build (const char *title, const char *body);

/* APPRISE_URLS = @url (char * → char *). */
GHashTable *bro_apprise_args_environment (const char *url);

G_END_DECLS
