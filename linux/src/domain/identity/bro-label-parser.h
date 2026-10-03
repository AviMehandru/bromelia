/* bro-label-parser.h: BroLabelParser, reads disc labels. */
#pragma once

#include "bro-label.h"

G_BEGIN_DECLS

/* Splits a label into words, reads set markers (S2, SEASON 2, P7, VOL 3, D2, DISC 2, S1D2 …) and returns the
 * cleaned title. Everything from the first set marker on is left out of the title. */
BroLabel *bro_label_parser_parse (const char *label);

/* "Season 2 Part 7 Disc 2", or "" when the label has no set information. Part of archive names, so never
 * translated. Free with g_free. */
char *bro_label_parser_set_description (const BroLabel *label);

G_END_DECLS
