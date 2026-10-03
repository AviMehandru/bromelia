/* bro-title-and-body.h: a notification's title and the lines of its body. */
#pragma once

#include "bro-bro-message.h"

G_BEGIN_DECLS

typedef struct {
  BroBroMessage *title;
  GPtrArray *lines; /* BroBroMessage * */
} BroTitleAndBody;

/* Takes ownership of @title; no lines yet. */
BroTitleAndBody *bro_title_and_body_new (BroBroMessage *title);
void bro_title_and_body_free (BroTitleAndBody *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroTitleAndBody, bro_title_and_body_free)

G_END_DECLS
