/* bro-label.h: what a disc label such as ONE_PIECE_S2_P7_D2 says: the title, the place in a set (-1: none), and
 * whether it looks like part of a series. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *title;
  int season, part, volume, disc; /* -1: not in the label (a season may be 0) */
  gboolean looks_like_series;
} BroLabel;

BroLabel *bro_label_copy (const BroLabel *label);
void bro_label_free (BroLabel *label);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLabel, bro_label_free)

G_END_DECLS
