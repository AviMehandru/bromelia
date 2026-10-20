/* bro-test-english.c */
#include "bro-test-english.h"
#include "bro-english-messages.h"
#include "bro-message-text-renderer.h"

char *
bro_test_english (const BroJsonValue *message)
{
  static BroMessageTextRenderer *renderer;
  static gsize once;
  if (g_once_init_enter (&once)) {
    renderer = bro_message_text_renderer_new (bro_english_messages_catalog ());
    g_once_init_leave (&once, 1);
  }
  return bro_message_text_renderer_render_json (renderer, message);
}
