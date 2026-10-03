/* bro-menu-numbers.h: BroMenuNumbers, episode numbers read (by OCR) from a DVD's menus. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The numbers (int) after "Episode" (OCR may read the O as 0), in the order they appear, each once. */
GArray *bro_menu_numbers_parse (const char *text);

/* The first episode S such that [S, S+count) covers the most of @numbers (int); FALSE without a unique best
 * choice backed by at least two numbers. */
gboolean bro_menu_numbers_first_episode (GArray *numbers, int count, int *out);

G_END_DECLS
