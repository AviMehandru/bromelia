/* bro-commit-intent.h: A row of commit_intents with its items: rolled forward at startup (plan §22.3). */
#pragma once

#include "bro-commit-item.h"
#include "bro-id.h"
#include "bro-instant.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId unit_id;
  BroId job_id;
  char *staging;
  char *destination;
  gboolean merge;
  gboolean quarantine;
  BroInstant created_at;
  GPtrArray *items; /* BroCommitItem * */
} BroCommitIntent;

/* Empty lists, nothing else set. */
BroCommitIntent *bro_commit_intent_new (void);
void bro_commit_intent_free (BroCommitIntent *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCommitIntent, bro_commit_intent_free)

G_END_DECLS
