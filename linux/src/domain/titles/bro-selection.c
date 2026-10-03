/* bro-selection.c */
#include "bro-selection.h"

void
bro_selection_free (BroSelection *selection)
{
  if (!selection)
    return;
  g_array_unref (selection->indices);
  g_ptr_array_unref (selection->trace);
  bro_bro_message_free (selection->error);
  g_free (selection);
}
