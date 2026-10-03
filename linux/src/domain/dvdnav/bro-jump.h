/* bro-jump.h: a decoded DVD VM jump, with its condition as text (empty when it has none). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_JUMP_PTT,   /* to a chapter: of VTS title title_number (JumpVTS_PTT) or of the current title (LinkPTTN,
                   * has_title_number FALSE) */
  BRO_JUMP_TITLE, /* to disc title number (JumpTT) */
} BroJumpKind;

typedef struct {
  BroJumpKind kind;
  gboolean has_title_number;
  int title_number;
  int chapter;
  int number;
  char *condition;
} BroJump;

void bro_jump_free (BroJump *jump);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroJump, bro_jump_free)

G_END_DECLS
