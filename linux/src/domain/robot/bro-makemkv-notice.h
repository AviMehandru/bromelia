/* bro-makemkv-notice.h: messages about the drive and about MakeMKV itself that Bromelia shows outside the log. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_MAKEMKV_NOTICE_LIBRE_DRIVE,            /* "Using LibreDrive mode (v06.3 id=…)"; detail holds what's in brackets */
  BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED,   /* the disc needs a LibreDrive-compatible drive, and this one isn't */
  BRO_MAKEMKV_NOTICE_KEY_EXPIRED,            /* the evaluation period or beta key has expired (5052, 5055) */
  BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED, /* makemkvcon can't start the evaluation */
  BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD,        /* MakeMKV needs updating, or a purchased key */
} BroMakemkvNoticeKind;

typedef struct {
  BroMakemkvNoticeKind kind;
  char *detail; /* LIBRE_DRIVE only; else "" */
} BroMakemkvNotice;

BroMakemkvNotice *bro_makemkv_notice_new (BroMakemkvNoticeKind kind, const char *detail);
BroMakemkvNotice *bro_makemkv_notice_copy (const BroMakemkvNotice *notice);
void bro_makemkv_notice_free (BroMakemkvNotice *notice);

/* MakeMKV can't (fully) work until the user acts: a key, an update or starting the evaluation. */
gboolean bro_makemkv_notice_is_license_problem (const BroMakemkvNotice *notice);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroMakemkvNotice, bro_makemkv_notice_free)

G_END_DECLS
