/* bro-output-source.h: which output a line came from. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_OUTPUT_SOURCE_STDOUT,
  BRO_OUTPUT_SOURCE_STDERR,
} BroOutputSource;

/* The wire form ("stdout"). */
const char *bro_output_source_to_wire (BroOutputSource value);
gboolean bro_output_source_from_wire (const char *text, BroOutputSource *out);

G_END_DECLS
