/* bro-cancellation-source.h: what cancels a BroCancellationToken: the job (or whoever started the work) keeps the
 * source and hands out its token, so what receives the token can't cancel the caller's work. A linked source is
 * cancelled with its parent too, until it is closed (or freed): closing removes its handler from the parent, so a
 * parent that lives as long as the daemon doesn't keep one per job. */
#pragma once

#include "bro-cancellation-token.h"

G_BEGIN_DECLS

typedef struct _BroCancellationSource BroCancellationSource;

BroCancellationSource *bro_cancellation_source_new (void);

/* A source that is cancelled when @parent is, or when it is cancelled itself. */
BroCancellationSource *bro_cancellation_source_linked (BroCancellationToken *parent);

/* Its token: valid as long as the source is (bro_cancellation_token_ref it to keep it longer). */
BroCancellationToken *bro_cancellation_source_token (BroCancellationSource *source);

/* Cancels the token and runs its handlers once, on the calling thread. */
void bro_cancellation_source_cancel (BroCancellationSource *source);

/* No longer follows the parent (a linked source); the token keeps its state. */
void bro_cancellation_source_close (BroCancellationSource *source);

/* Closes it, and drops its reference to the token. */
void bro_cancellation_source_free (BroCancellationSource *source);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCancellationSource, bro_cancellation_source_free)

G_END_DECLS
