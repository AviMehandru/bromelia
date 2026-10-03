/* bro-argument-splitter.h: BroArgumentSplitter, splits a command line like a POSIX shell (quotes and backslash
 * escapes), without expansion. The same on every platform, so configurations stay portable. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Free with g_strfreev. */
GStrv bro_argument_splitter_split (const char *text);

/* An argument quoted for display, POSIX style on every platform: as is when it has only ASCII letters, digits and
 * -_./:=+,@%, else in single quotes (it's → 'it'\''s'). Free with g_free. */
char *bro_argument_splitter_quote (const char *argument);

G_END_DECLS
