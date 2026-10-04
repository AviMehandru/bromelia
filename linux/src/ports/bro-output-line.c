/* bro-output-line.c */
#include "bro-output-line.h"

BroOutputLine *
bro_output_line_new (void)
{
  BroOutputLine *x = g_new0 (BroOutputLine, 1);
  return x;
}

void
bro_output_line_free (BroOutputLine *x)
{
  if (!x)
    return;
  g_free (x->text);
  g_free (x);
}
