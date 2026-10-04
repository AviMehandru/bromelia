/* bro-output-line.h: A line of a process's output, without its line break, and when it was read. */
#pragma once

#include "bro-instant.h"
#include "bro-output-source.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroOutputSource stream;
  char *text;
  BroInstant at;
} BroOutputLine;

/* Everything zero. */
BroOutputLine *bro_output_line_new (void);
void bro_output_line_free (BroOutputLine *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroOutputLine, bro_output_line_free)

G_END_DECLS
