/* bro-tool-info.c */
#include "bro-tool-info.h"

BroToolInfo *
bro_tool_info_new (void)
{
  BroToolInfo *x = g_new0 (BroToolInfo, 1);
  x->capabilities = g_new0 (char *, 1);
  return x;
}

void
bro_tool_info_free (BroToolInfo *x)
{
  if (!x)
    return;
  g_free (x->path);
  g_free (x->version);
  g_strfreev (x->capabilities);
  g_clear_pointer (&x->why, bro_bro_message_free);
  g_free (x);
}
