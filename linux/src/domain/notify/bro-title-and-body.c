/* bro-title-and-body.c */
#include "bro-title-and-body.h"

BroTitleAndBody *
bro_title_and_body_new (BroBroMessage *title)
{
  BroTitleAndBody *v = g_new0 (BroTitleAndBody, 1);
  v->title = title;
  v->lines = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_bro_message_free);
  return v;
}

void
bro_title_and_body_free (BroTitleAndBody *value)
{
  if (!value)
    return;
  bro_bro_message_free (value->title);
  g_ptr_array_unref (value->lines);
  g_free (value);
}
