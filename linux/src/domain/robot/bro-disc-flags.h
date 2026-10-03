/* bro-disc-flags.h: the disc flags of a DRV line: which file systems MakeMKV found. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int raw; /* 1 dvdFiles, 2 hdDvdFiles, 4 blurayFiles, 8 aacsFiles, 16 bdsvmFiles */
} BroDiscFlags;

#define BRO_DISC_FLAGS_DVD_FILES 1
#define BRO_DISC_FLAGS_HD_DVD_FILES 2
#define BRO_DISC_FLAGS_BLURAY_FILES 4
#define BRO_DISC_FLAGS_AACS_FILES 8
#define BRO_DISC_FLAGS_BDSVM_FILES 16

/* MakeMKV's disc type: "Blu-ray (AACS)", "Blu-ray", "HD DVD", "DVD" or "Disc". */
const char *bro_disc_flags_type_text (BroDiscFlags flags);

G_END_DECLS
