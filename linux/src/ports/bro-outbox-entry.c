/* bro-outbox-entry.c */
#include "bro-outbox-entry.h"

BroOutboxEntry *
bro_outbox_entry_new (void)
{
  BroOutboxEntry *x = g_new0 (BroOutboxEntry, 1);
  x->lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  return x;
}

void
bro_outbox_entry_free (BroOutboxEntry *x)
{
  if (!x)
    return;
  g_free (x->target_id);
  g_clear_pointer (&x->title, bro_bro_message_free);
  g_clear_pointer (&x->lines, g_ptr_array_unref);
  g_clear_pointer (&x->last_error, bro_bro_error_free);
  g_free (x);
}
