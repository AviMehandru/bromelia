/* bro-job-page-dto.c */
#include "bro-job-page-dto.h"

BroJobPageDto *
bro_job_page_dto_new (void)
{
  BroJobPageDto *p = g_new0 (BroJobPageDto, 1);
  p->items = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_job_dto_free);
  return p;
}

void
bro_job_page_dto_free (BroJobPageDto *p)
{
  if (!p)
    return;
  g_ptr_array_unref (p->items);
  g_free (p->next);
  g_free (p);
}

BroJsonValue *
bro_job_page_dto_json (const BroJobPageDto *page)
{
  BroJsonValue *o = bro_json_value_new_object ();
  BroJsonValue *items = bro_json_value_new_array ();
  for (guint i = 0; i < page->items->len; i++)
    bro_json_value_append (items, bro_job_dto_json (g_ptr_array_index (page->items, i)));
  bro_json_value_set (o, "items", items);
  bro_json_value_set (o, "next", page->next ? bro_json_value_new_string (page->next) : bro_json_value_new_null ());
  bro_json_value_set (o, "total", bro_json_value_new_integer (page->total));
  return o;
}
