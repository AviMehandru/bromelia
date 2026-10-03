/* bro-job-kind.h */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_JOB_KIND_VIDEO_DISC,
  BRO_JOB_KIND_AUDIO_CD,
  BRO_JOB_KIND_DATA_DISC,
  BRO_JOB_KIND_READ_ERROR_RETRY,
  BRO_JOB_KIND_VERIFY,
  BRO_JOB_KIND_REPLICATE,
  BRO_JOB_KIND_PARITY,
  BRO_JOB_KIND_REPAIR,
  BRO_JOB_KIND_RESCAN,
  BRO_JOB_KIND_RUN_COMMAND,
  BRO_JOB_KIND_TRANSCODE,
} BroJobKind;

/* The wire form ("videoDisc"), as in shared/schema/common.json. */
const char *bro_job_kind_to_wire (BroJobKind value);
gboolean bro_job_kind_from_wire (const char *text, BroJobKind *out);

G_END_DECLS
