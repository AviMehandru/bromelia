/* bro-space-estimate.c */
#include "bro-space-estimate.h"

BroBytes
bro_space_estimate_required (BroBytes bytes)
{
  BroBytes b = { bytes.count + MAX ((gint64) 256 << 20, bytes.count / 50) };
  return b;
}

BroBytes
bro_space_estimate_for_plan (GPtrArray *titles, GArray *hand_picked, int split_title)
{
  gint64 need = 0;
  for (guint i = 0; i < titles->len; i++) {
    const BroTitle *t = titles->pdata[i];
    need += t->size_bytes;
    for (guint k = 0; hand_picked && k < hand_picked->len; k++)
      if (g_array_index (hand_picked, int, k) == t->index) {
        need += t->size_bytes;
        break;
      }
  }
  for (guint i = 0; split_title >= 0 && i < titles->len; i++) {
    const BroTitle *t = titles->pdata[i];
    if (t->index == split_title) {
      need += t->size_bytes;
      break;
    }
  }
  BroBytes b = { need };
  return b;
}
