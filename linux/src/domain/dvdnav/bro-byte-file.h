/* bro-byte-file.h: a file of a BroByteSource: its upper-case name and size in bytes. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *name;
  gint64 size;
} BroByteFile;

BroByteFile *bro_byte_file_new (const char *name, gint64 size);
void bro_byte_file_free (BroByteFile *file);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroByteFile, bro_byte_file_free)

G_END_DECLS
