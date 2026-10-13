/* bro-library-dto.h: BroLibraryDto: a library, as clients see it (dtos.json Library): online, or the reason it isn't;
 * the free space when it is; when its marker was last seen. */
#pragma once

#include "bro-bro-message.h"
#include "bro-instant.h"

G_BEGIN_DECLS

typedef struct {
  char *id;
  char *name;
  char *path;
  gboolean online;
  BroBroMessage *reason; /* NULL: none */
  gboolean has_free_bytes;
  gint64 free_bytes;
  gboolean has_last_seen_at;
  BroInstant last_seen_at;
} BroLibraryDto;

/* Everything zero. */
BroLibraryDto *bro_library_dto_new (void);
void bro_library_dto_free (BroLibraryDto *library);

/* Optional members left out when absent. Free with bro_json_value_unref. */
BroJsonValue *bro_library_dto_json (const BroLibraryDto *library);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroLibraryDto, bro_library_dto_free)

G_END_DECLS
