/* bro-cancellation-token.h: cancellation that crosses layers (plan §6). Thread-safe and reference counted.
 * Built on GLib's core only, so Domain stays free of GIO; the adapters bridge it to GCancellable. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _BroCancellationToken BroCancellationToken;

BroCancellationToken *bro_cancellation_token_new (void);
BroCancellationToken *bro_cancellation_token_ref (BroCancellationToken *token);
void bro_cancellation_token_unref (BroCancellationToken *token);

/* A token that is cancelled when @parent is, or when it is cancelled itself. */
BroCancellationToken *bro_cancellation_token_child (BroCancellationToken *parent);

gboolean bro_cancellation_token_is_cancelled (BroCancellationToken *token);

/* Cancels the token and runs its handlers once, on the calling thread. */
void bro_cancellation_token_cancel (BroCancellationToken *token);

/* Runs @handler (@data) once on cancellation; straight away, returning 0, when already cancelled. Otherwise
 * returns an id for bro_cancellation_token_disconnect. @destroy frees @data when the handler is dropped. */
guint bro_cancellation_token_on_cancel (BroCancellationToken *token, GFunc handler, gpointer data, GDestroyNotify destroy);
void bro_cancellation_token_disconnect (BroCancellationToken *token, guint id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroCancellationToken, bro_cancellation_token_unref)

G_END_DECLS
