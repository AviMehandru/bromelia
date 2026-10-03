/* bro-media-args-private.h: helpers shared inside MediaArgs. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* "~" and "~/…" (or "~\…") start in @home; anything else, or no home, stays as it is. */
char *_bro_media_args_expand_home (const char *path, const char *home);

G_END_DECLS
