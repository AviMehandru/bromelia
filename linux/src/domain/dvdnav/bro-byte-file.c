/* bro-byte-file.c */
#include "bro-byte-file.h"

BroByteFile *
bro_byte_file_new (const char *name, gint64 size)
{
  BroByteFile *f = g_new0 (BroByteFile, 1);
  f->name = g_strdup (name);
  f->size = size;
  return f;
}

void
bro_byte_file_free (BroByteFile *file)
{
  if (!file)
    return;
  g_free (file->name);
  g_free (file);
}
