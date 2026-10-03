/* bro-label.c */
#include "bro-label.h"

BroLabel *
bro_label_copy (const BroLabel *label)
{
  if (!label)
    return NULL;
  BroLabel *l = g_new0 (BroLabel, 1);
  *l = *label;
  l->title = g_strdup (label->title);
  return l;
}

void
bro_label_free (BroLabel *label)
{
  if (!label)
    return;
  g_free (label->title);
  g_free (label);
}
