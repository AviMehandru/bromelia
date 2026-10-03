/* bro-disc-flags.c */
#include "bro-disc-flags.h"

const char *
bro_disc_flags_type_text (BroDiscFlags flags)
{
  if (flags.raw & BRO_DISC_FLAGS_BLURAY_FILES)
    return flags.raw & BRO_DISC_FLAGS_AACS_FILES ? "Blu-ray (AACS)" : "Blu-ray";
  if (flags.raw & BRO_DISC_FLAGS_HD_DVD_FILES)
    return "HD DVD";
  if (flags.raw & BRO_DISC_FLAGS_DVD_FILES)
    return "DVD";
  return "Disc";
}
