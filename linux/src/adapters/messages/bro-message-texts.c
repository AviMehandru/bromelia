/* bro-message-texts.c */
#include "bro-message-texts.h"

struct _BroMessageTexts {
  GHashTable *texts; /* code → text */
  GHashTable *lists; /* "code\x1fparam" */
};

static char *
list_key (const char *code, const char *param)
{
  return g_strconcat (code, "\x1f", param, NULL);
}

BroMessageTexts *
bro_message_texts_new (void)
{
  BroMessageTexts *self = g_new0 (BroMessageTexts, 1);
  self->texts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->lists = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return self;
}

void
bro_message_texts_free (BroMessageTexts *self)
{
  if (!self)
    return;
  g_hash_table_unref (self->texts);
  g_hash_table_unref (self->lists);
  g_free (self);
}

void
bro_message_texts_add_text (BroMessageTexts *self, const char *code, const char *text)
{
  g_hash_table_replace (self->texts, g_strdup (code), g_strdup (text));
}

void
bro_message_texts_add_message_list (BroMessageTexts *self, const char *code, const char *param)
{
  g_hash_table_add (self->lists, list_key (code, param));
}

const char *
bro_message_texts_text (const BroMessageTexts *self, const char *code)
{
  return g_hash_table_lookup (self->texts, code);
}

gboolean
bro_message_texts_is_message_list (const BroMessageTexts *self, const char *code, const char *param)
{
  g_autofree char *key = list_key (code, param);
  return g_hash_table_contains (self->lists, key);
}
