/* bro-cancellation-source.c */
#include "bro-cancellation-source.h"

struct _BroCancellationSource {
  BroCancellationToken *token;
  GMutex lock;
  BroCancellationToken *parent; /* nullable: a linked source until it is closed */
  guint parent_handler;
};

BroCancellationSource *
bro_cancellation_source_new (void)
{
  BroCancellationSource *source = g_new0 (BroCancellationSource, 1);
  source->token = _bro_cancellation_token_new ();
  g_mutex_init (&source->lock);
  return source;
}

static void
cancel_linked (gpointer token, gpointer parent)
{
  _bro_cancellation_token_cancel (token);
}

BroCancellationSource *
bro_cancellation_source_linked (BroCancellationToken *parent)
{
  BroCancellationSource *source = bro_cancellation_source_new ();
  guint id = bro_cancellation_token_on_cancel (parent, cancel_linked, bro_cancellation_token_ref (source->token),
                                               (GDestroyNotify) bro_cancellation_token_unref);
  if (id)
    {
      source->parent = bro_cancellation_token_ref (parent);
      source->parent_handler = id;
    }
  return source;
}

BroCancellationToken *
bro_cancellation_source_token (BroCancellationSource *source)
{
  return source->token;
}

void
bro_cancellation_source_cancel (BroCancellationSource *source)
{
  _bro_cancellation_token_cancel (source->token);
}

void
bro_cancellation_source_close (BroCancellationSource *source)
{
  BroCancellationToken *parent;
  guint id;
  g_mutex_lock (&source->lock);
  parent = g_steal_pointer (&source->parent);
  id = source->parent_handler;
  source->parent_handler = 0;
  g_mutex_unlock (&source->lock);
  if (parent)
    {
      bro_cancellation_token_disconnect (parent, id);
      bro_cancellation_token_unref (parent);
    }
}

void
bro_cancellation_source_free (BroCancellationSource *source)
{
  if (!source)
    return;
  bro_cancellation_source_close (source);
  bro_cancellation_token_unref (source->token);
  g_mutex_clear (&source->lock);
  g_free (source);
}
