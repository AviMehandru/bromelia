/* bro-medium-dto.h: BroMediumDto: the disc in a drive (dtos.json Medium): its generation (the drive's discs counted
 * since the engine started), label, type (dvd, bd, hddvd or disc, from MakeMKV's flags), MakeMKV's type text, flags,
 * what is on it and where the OS mounted it. */
#pragma once

#include "bro-disc-content.h"
#include "bro-disc-flags.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct {
  gint64 generation;
  char *label;
  char *type;
  char *type_text;
  GPtrArray *flags; /* char *, dtos.json order */
  BroDiscContent content;
  char *mount_path; /* NULL: not mounted */
} BroMediumDto;

/* Takes no ownership: the strings are copied; @flags of BroMediumDto.flags come from @raw_flags. */
BroMediumDto *bro_medium_dto_new (gint64 generation, const char *label, BroDiscFlags raw_flags, BroDiscContent content, const char *mount_path);
BroMediumDto *bro_medium_dto_copy (const BroMediumDto *medium);
void bro_medium_dto_free (BroMediumDto *medium);

/* The flags' names (dvdFiles, hdDvdFiles, blurayFiles, aacsFiles, bdsvmFiles, in that order): char *, the caller frees
 * the array. */
GPtrArray *bro_medium_dto_flag_names (BroDiscFlags flags);

/* The {type} of a disc with these flags (common.json DiscType, from FormatDetector): "dvd", "bd", "hddvd" or "disc". */
const char *bro_medium_dto_type_of (BroDiscFlags flags);

/* Free with bro_json_value_unref. */
BroJsonValue *bro_medium_dto_json (const BroMediumDto *medium);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMediumDto, bro_medium_dto_free)

G_END_DECLS
