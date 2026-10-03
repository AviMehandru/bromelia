/* bro-format-code.h: the format code of file names: DVD, BR, 4K, HDDVD or DISC, with an 'e' suffix for backups
 * that weren't decrypted (common.json's FormatCode). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char text[8];
} BroFormatCode;

G_END_DECLS
