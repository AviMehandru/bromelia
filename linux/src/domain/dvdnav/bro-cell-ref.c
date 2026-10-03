/* bro-cell-ref.c */
#include "bro-cell-ref.h"

BroCellRef *
bro_cell_ref_new (const char *file, gint64 first_sector, gint64 end_sector)
{
  BroCellRef *c = g_new0 (BroCellRef, 1);
  c->file = g_strdup (file);
  c->first_sector = first_sector;
  c->end_sector = end_sector;
  return c;
}

void
bro_cell_ref_free (BroCellRef *cell)
{
  if (!cell)
    return;
  g_free (cell->file);
  g_free (cell);
}
