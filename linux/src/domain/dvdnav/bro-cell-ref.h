/* bro-cell-ref.h: a menu still: the sectors [first, end) of one VOB/cell id in a menu VOB (input for MenuOcr). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *file;
  gint64 first_sector;
  gint64 end_sector;
} BroCellRef;

BroCellRef *bro_cell_ref_new (const char *file, gint64 first_sector, gint64 end_sector);
void bro_cell_ref_free (BroCellRef *cell);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCellRef, bro_cell_ref_free)

G_END_DECLS
