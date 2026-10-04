/* bro-commit-item.h: A row of commit_items. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int seq;
  char *from_path; /* relative to staging */
  char *to_path; /* relative to the destination */
  char *sha256; /* nullable */
  gboolean is_dir;
  gboolean moved;
} BroCommitItem;

/* Everything zero. */
BroCommitItem *bro_commit_item_new (void);
void bro_commit_item_free (BroCommitItem *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCommitItem, bro_commit_item_free)

G_END_DECLS
