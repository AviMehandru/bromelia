/* bro-folder-check.h: a checked archive folder and its result. Borrowed. */
#pragma once

#include "bro-verify-result.h"

G_BEGIN_DECLS

typedef struct {
  const char *folder;
  const BroVerifyResult *result;
} BroFolderCheck;

G_END_DECLS
