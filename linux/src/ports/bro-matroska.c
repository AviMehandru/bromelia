/* bro-matroska.c */
#include "bro-matroska.h"

G_DEFINE_INTERFACE (BroMatroska, bro_matroska, G_TYPE_OBJECT)

static void
bro_matroska_default_init (BroMatroskaInterface *iface)
{
}

BroMkvProbe *
bro_matroska_probe (BroMatroska *self, const char *file, BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MATROSKA_GET_IFACE (self)->probe (self, file, cancel, error);
}

gboolean
bro_matroska_remux (BroMatroska *self, const char *const *arguments, BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MATROSKA_GET_IFACE (self)->remux (self, arguments, cancel, error);
}

GStrv
bro_matroska_split (BroMatroska *self, GArray *chapters, const char *input, BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MATROSKA_GET_IFACE (self)->split (self, chapters, input, cancel, error);
}

GArray *
bro_matroska_chapter_times (BroMatroska *self, const char *file, BroCancellationToken *cancel, BroBroError **error)
{
  return BRO_MATROSKA_GET_IFACE (self)->chapter_times (self, file, cancel, error);
}
