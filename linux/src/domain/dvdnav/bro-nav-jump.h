/* bro-nav-jump.h: how a chapter of a title is reached (the first way found): a menu, a button or a pre-command. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int title;
  int chapter;
  char *how;
} BroNavJump;

BroNavJump *bro_nav_jump_new (int title, int chapter, const char *how);
void bro_nav_jump_free (BroNavJump *jump);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroNavJump, bro_nav_jump_free)

G_END_DECLS
