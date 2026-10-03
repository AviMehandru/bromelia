/* bro-makemkv-notice.c */
#include "bro-makemkv-notice.h"

BroMakemkvNotice *
bro_makemkv_notice_new (BroMakemkvNoticeKind kind, const char *detail)
{
  BroMakemkvNotice *n = g_new0 (BroMakemkvNotice, 1);
  n->kind = kind;
  n->detail = g_strdup (detail ? detail : "");
  return n;
}

BroMakemkvNotice *
bro_makemkv_notice_copy (const BroMakemkvNotice *notice)
{
  return notice ? bro_makemkv_notice_new (notice->kind, notice->detail) : NULL;
}

void
bro_makemkv_notice_free (BroMakemkvNotice *notice)
{
  if (!notice)
    return;
  g_free (notice->detail);
  g_free (notice);
}

gboolean
bro_makemkv_notice_is_license_problem (const BroMakemkvNotice *notice)
{
  return notice->kind == BRO_MAKEMKV_NOTICE_KEY_EXPIRED || notice->kind == BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED
         || notice->kind == BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD;
}
