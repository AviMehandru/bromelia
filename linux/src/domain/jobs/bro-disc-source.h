/* bro-disc-source.h: where a video-disc job reads (dtos.json's SessionSource): a drive, a disc image, a folder holding
 * a disc structure, or a file inside one (SourceResolver opens the disc it belongs to). */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef enum {
  BRO_DISC_SOURCE_DRIVE,  /* value: the drive id */
  BRO_DISC_SOURCE_ISO,    /* value: the path */
  BRO_DISC_SOURCE_FOLDER, /* value: the path */
  BRO_DISC_SOURCE_FILE,   /* value: the path */
} BroDiscSourceKind;

typedef struct {
  BroDiscSourceKind kind;
  char *value;
} BroDiscSource;

BroDiscSource *bro_disc_source_new (BroDiscSourceKind kind, const char *value);
BroDiscSource *bro_disc_source_copy (const BroDiscSource *source);
void bro_disc_source_free (BroDiscSource *source);

/* {"drive": id}, {"iso": path}, {"folder": path} or {"file": path}. */
BroJsonValue *bro_disc_source_encode (const BroDiscSource *source);

/* The source bro_disc_source_encode wrote; NULL for anything else. */
BroDiscSource *bro_disc_source_decode (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDiscSource, bro_disc_source_free)

G_END_DECLS
