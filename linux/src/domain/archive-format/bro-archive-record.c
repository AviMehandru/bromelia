/* bro-archive-record.c */
#include "bro-archive-record.h"

void
bro_archive_record_free (BroArchiveRecord *r)
{
  if (!r)
    return;
  bro_json_value_unref (r->document);
  g_free (r->status);
  g_free (r->name);
  g_free (r->kind);
  g_free (r->label);
  g_free (r->volume_name);
  g_free (r->fingerprint);
  g_free (r->unit_id);
  g_array_unref (r->episodes);
  g_ptr_array_unref (r->files);
  g_free (r);
}
