/* bro-chapter-range.h: the first and last disc chapter of an episode. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int first;
  int last;
} BroChapterRange;

G_END_DECLS
