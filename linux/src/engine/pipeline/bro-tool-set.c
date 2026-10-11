/* bro-tool-set.c */
#include "bro-tool-set.h"

BroToolSet *
bro_tool_set_new (BroMakemkv *makemkv, BroMatroska *matroska)
{
  BroToolSet *t = g_new0 (BroToolSet, 1);
  t->makemkv = makemkv ? g_object_ref (makemkv) : NULL;
  t->matroska = matroska ? g_object_ref (matroska) : NULL;
  return t;
}

BroToolSet *
bro_tool_set_copy (const BroToolSet *tools)
{
  return tools ? bro_tool_set_new (tools->makemkv, tools->matroska) : NULL;
}

void
bro_tool_set_free (BroToolSet *tools)
{
  if (!tools)
    return;
  g_clear_object (&tools->makemkv);
  g_clear_object (&tools->matroska);
  g_free (tools);
}
