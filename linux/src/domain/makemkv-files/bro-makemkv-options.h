/* bro-makemkv-options.h: the switches every makemkvcon run gets: the profile file (NULL: none), --minlength,
 * --cache and --directio (not passed when negative / has_direct_io is FALSE), more arguments (split like a POSIX
 * command line; nullable), and whether to scan drives (FALSE: --noscan). Borrowed; BRO_MAKEMKV_OPTIONS_NONE passes
 * nothing but --noscan. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  const char *profile_path;
  int min_length_seconds; /* -1: not passed */
  int cache_mb;           /* 0 or less: not passed */
  gboolean has_direct_io;
  gboolean direct_io;
  const char *extra_arguments;
  gboolean scan;
} BroMakemkvOptions;

#define BRO_MAKEMKV_OPTIONS_NONE ((BroMakemkvOptions) { NULL, -1, 0, FALSE, FALSE, NULL, FALSE })

G_END_DECLS
