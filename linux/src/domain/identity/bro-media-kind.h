/* bro-media-kind.h: a movie or a TV show */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_MEDIA_KIND_MOVIE,
  BRO_MEDIA_KIND_TV,
} BroMediaKind;

/* The wire form ("movie"), as in shared/schema/common.json. */
const char *bro_media_kind_to_wire (BroMediaKind value);
gboolean bro_media_kind_from_wire (const char *text, BroMediaKind *out);

G_END_DECLS
