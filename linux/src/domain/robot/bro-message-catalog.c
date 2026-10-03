/* bro-message-catalog.c */
#include "bro-message-catalog.h"
#include "bro-robot-private.h"

#include <string.h>

/* 2018: write error (e.g. "No space left on device"); 5006: the source file doesn't exist. */
static const int error_codes[] = { 2003, 2004, 2018, 2023, 5003, 5006, 5010, 5021, 5037, 5055, 5069, 5077 };
static const int warning_codes[] = { 3038, 3041, 5042 };

static gboolean
in (const int *codes, gsize n, int code)
{
  for (gsize i = 0; i < n; i++)
    if (codes[i] == code)
      return TRUE;
  return FALSE;
}

BroMessageKind
bro_message_catalog_severity (const BroRobotMessage *message)
{
  if (message->code == 1003 || ((message->flags & 0x20) != 0 && g_str_has_prefix (message->text, "DEBUG")))
    return BRO_MESSAGE_KIND_DEBUG;
  if ((message->flags & 0x200) != 0 || in (error_codes, G_N_ELEMENTS (error_codes), message->code))
    return BRO_MESSAGE_KIND_ERROR;
  if ((message->flags & 0x400) != 0 || in (warning_codes, G_N_ELEMENTS (warning_codes), message->code))
    return BRO_MESSAGE_KIND_WARNING;
  return BRO_MESSAGE_KIND_INFO;
}

BroMakemkvNotice *
bro_message_catalog_notice (const BroRobotMessage *message)
{
  const char *t = message->text;
  if (g_str_has_prefix (t, "Using LibreDrive mode")) {
    const char *open = strchr (t, '('), *close = strrchr (t, ')');
    g_autofree char *detail = open && close && close > open ? g_strndup (open + 1, close - open - 1) : g_strdup ("");
    return bro_makemkv_notice_new (BRO_MAKEMKV_NOTICE_LIBRE_DRIVE, detail);
  }
  if (strstr (t, "LibreDrive compatible drive is required"))
    return bro_makemkv_notice_new (BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED, NULL);
  g_autofree char *lower = _bro_robot_ascii_lower (t);
  if (message->code == 5052 || message->code == 5055 || strstr (lower, "evaluation period has expired")
      || strstr (lower, "evaluation period expired"))
    return bro_makemkv_notice_new (BRO_MAKEMKV_NOTICE_KEY_EXPIRED, NULL);
  if (strstr (t, "Evaluation period not started") || strstr (t, "start MakeMKV evaluation from a third-party application"))
    return bro_makemkv_notice_new (BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED, NULL);
  if (strstr (t, "application version is too old"))
    return bro_makemkv_notice_new (BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD, NULL);
  return NULL;
}
