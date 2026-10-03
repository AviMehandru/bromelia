/* bro-cancellation-token.c */
#include "bro-cancellation-token.h"

typedef struct {
  guint id;
  GFunc handler;
  gpointer data;
  GDestroyNotify destroy;
} Handler;

struct _BroCancellationToken {
  GMutex lock;
  gboolean cancelled;
  GArray *handlers; /* Handler */
  guint next_id;
};

static void
drop_handler (Handler *h)
{
  if (h->destroy)
    h->destroy (h->data);
}

BroCancellationToken *
bro_cancellation_token_new (void)
{
  BroCancellationToken *t = g_atomic_rc_box_new0 (BroCancellationToken);
  g_mutex_init (&t->lock);
  t->handlers = g_array_new (FALSE, FALSE, sizeof (Handler));
  return t;
}

BroCancellationToken *
bro_cancellation_token_ref (BroCancellationToken *token)
{
  return g_atomic_rc_box_acquire (token);
}

static void
clear_token (BroCancellationToken *t)
{
  for (guint i = 0; i < t->handlers->len; i++)
    drop_handler (&g_array_index (t->handlers, Handler, i));
  g_array_unref (t->handlers);
  g_mutex_clear (&t->lock);
}

void
bro_cancellation_token_unref (BroCancellationToken *token)
{
  if (token)
    g_atomic_rc_box_release_full (token, (GDestroyNotify) clear_token);
}

gboolean
bro_cancellation_token_is_cancelled (BroCancellationToken *token)
{
  g_mutex_lock (&token->lock);
  gboolean c = token->cancelled;
  g_mutex_unlock (&token->lock);
  return c;
}

void
bro_cancellation_token_cancel (BroCancellationToken *token)
{
  g_mutex_lock (&token->lock);
  if (token->cancelled) {
    g_mutex_unlock (&token->lock);
    return;
  }
  token->cancelled = TRUE;
  GArray *run = token->handlers;
  token->handlers = g_array_new (FALSE, FALSE, sizeof (Handler));
  g_mutex_unlock (&token->lock);
  for (guint i = 0; i < run->len; i++) {
    Handler *h = &g_array_index (run, Handler, i);
    h->handler (h->data, token);
    drop_handler (h);
  }
  g_array_unref (run);
}

guint
bro_cancellation_token_on_cancel (BroCancellationToken *token, GFunc handler, gpointer data, GDestroyNotify destroy)
{
  g_mutex_lock (&token->lock);
  if (token->cancelled) {
    g_mutex_unlock (&token->lock);
    handler (data, token);
    if (destroy)
      destroy (data);
    return 0;
  }
  Handler h = { ++token->next_id, handler, data, destroy };
  g_array_append_val (token->handlers, h);
  g_mutex_unlock (&token->lock);
  return h.id;
}

void
bro_cancellation_token_disconnect (BroCancellationToken *token, guint id)
{
  Handler dropped = { 0 };
  g_mutex_lock (&token->lock);
  for (guint i = 0; i < token->handlers->len; i++)
    if (g_array_index (token->handlers, Handler, i).id == id) {
      dropped = g_array_index (token->handlers, Handler, i);
      g_array_remove_index (token->handlers, i);
      break;
    }
  g_mutex_unlock (&token->lock);
  if (dropped.id)
    drop_handler (&dropped);
}

static void
cancel_child (gpointer child, gpointer parent)
{
  bro_cancellation_token_cancel (child);
}

BroCancellationToken *
bro_cancellation_token_child (BroCancellationToken *parent)
{
  BroCancellationToken *child = bro_cancellation_token_new ();
  /* The parent holds a reference to the child until it is cancelled or freed. */
  bro_cancellation_token_on_cancel (parent, cancel_child, bro_cancellation_token_ref (child),
                                    (GDestroyNotify) bro_cancellation_token_unref);
  return child;
}
