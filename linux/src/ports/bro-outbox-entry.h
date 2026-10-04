/* bro-outbox-entry.h: A row of outbox: a notification waiting to be sent, and what happened to it. */
#pragma once

#include "bro-bro-error.h"
#include "bro-bro-message.h"
#include "bro-id.h"
#include "bro-instant.h"
#include "bro-status-word.h"
#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  BroId id;
  char *target_id;
  gboolean has_job_id;
  BroId job_id;
  BroBroMessage *title;
  GPtrArray *lines; /* BroBroMessage * * */
  BroStatusWord status;
  BroInstant created_at;
  int attempts;
  gboolean has_next_attempt_at;
  BroInstant next_attempt_at;
  gboolean has_sent_at;
  BroInstant sent_at;
  BroBroError *last_error; /* nullable */
} BroOutboxEntry;

/* Empty lists, nothing else set. */
BroOutboxEntry *bro_outbox_entry_new (void);
void bro_outbox_entry_free (BroOutboxEntry *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroOutboxEntry, bro_outbox_entry_free)

G_END_DECLS
