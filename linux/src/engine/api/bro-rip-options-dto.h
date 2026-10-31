/* bro-rip-options-dto.h: BroRipOptionsDto: how a client asks for a rip (dtos.json RipOptions): the mode, profile and
 * library to use instead of what the rules give, the titles of the open session, and a one-off output folder that must
 * be a library's. */
#pragma once

#include "bro-rip-mode.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gboolean has_mode;
  BroRipMode mode;
  char *profile;       /* nullable */
  char *library;       /* nullable */
  GArray *titles;      /* int; NULL: none chosen (an empty array is a choice) */
  char *output_folder; /* nullable */
} BroRipOptionsDto;

/* Nothing chosen. */
BroRipOptionsDto *bro_rip_options_dto_new (void);
BroRipOptionsDto *bro_rip_options_dto_copy (const BroRipOptionsDto *options);
void bro_rip_options_dto_free (BroRipOptionsDto *options);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRipOptionsDto, bro_rip_options_dto_free)

G_END_DECLS
