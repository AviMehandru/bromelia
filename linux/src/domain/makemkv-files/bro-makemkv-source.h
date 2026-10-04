/* bro-makemkv-source.h: a source makemkvcon can open: a drive (MakeMKV's index and the OS device, "" when unknown), a
 * disc image, or a folder holding a disc structure. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_MAKEMKV_SOURCE_DRIVE, /* index, device */
  BRO_MAKEMKV_SOURCE_ISO,   /* path */
  BRO_MAKEMKV_SOURCE_FILE,  /* path */
} BroMakemkvSourceKind;

typedef struct {
  BroMakemkvSourceKind kind;
  int index;
  char *device;
  char *path;
} BroMakemkvSource;

BroMakemkvSource *bro_makemkv_source_new (BroMakemkvSourceKind kind, int index, const char *device, const char *path);
void bro_makemkv_source_free (BroMakemkvSource *source);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMakemkvSource, bro_makemkv_source_free)

G_END_DECLS
