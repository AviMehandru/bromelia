/* bro-moved-item.h: an item moveMerging moved, and where it went. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *from;
  char *to;
} BroMovedItem;

/* Everything zero. */
BroMovedItem *bro_moved_item_new (void);
void bro_moved_item_free (BroMovedItem *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMovedItem, bro_moved_item_free)

G_END_DECLS
