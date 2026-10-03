/* bro-identity.h: what a disc is: the movie or show on it, movie or TV, its format and format code, whether the
 * backup stays encrypted, its label and why it's taken for a movie or a show. */
#pragma once

#include "bro-bro-message.h"
#include "bro-format-code.h"
#include "bro-identity-inputs.h"
#include "bro-label.h"

G_BEGIN_DECLS

typedef struct {
  char *name;
  BroMediaKind kind;
  BroDiscFormat format;
  BroFormatCode format_code;
  gboolean encrypted;
  BroLabel *label;
  BroBroMessage *reason;
} BroIdentity;

/* The name: typed by the user, else the disc's name when it looks written by a person ("The Dark Knight", not
 * DARK_KNIGHT_D1), else the label's title, else the label, else "Disc". The label merges the volume name's set
 * information with the disc name's. */
BroIdentity *bro_identity_resolve (const BroIdentityInputs *inputs);
void bro_identity_free (BroIdentity *identity);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroIdentity, bro_identity_free)

G_END_DECLS
