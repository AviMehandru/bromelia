/* bro-nav-title.h: a disc title: its number, chapter lengths in seconds and the VOB id of each chapter. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int number;
  GArray *chapters;     /* double */
  GArray *chapter_vobs; /* int */
} BroNavTitle;

/* No chapters. */
BroNavTitle *bro_nav_title_new (int number);
void bro_nav_title_free (BroNavTitle *title);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroNavTitle, bro_nav_title_free)

G_END_DECLS
