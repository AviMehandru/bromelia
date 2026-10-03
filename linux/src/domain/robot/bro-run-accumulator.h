/* bro-run-accumulator.h: what one makemkvcon run said, fed one event at a time: the saved / failed counts, the
 * errors and the first of them (usually the cause), MakeMKV's space warning, read errors, a renumbered drive,
 * the debug log, notices and the version. When stop_reason is set, the caller stops the process. Read the
 * fields directly. */
#pragma once

#include "bro-bro-message.h"
#include "bro-makemkv-notice.h"
#include "bro-robot-event.h"

G_BEGIN_DECLS

typedef struct {
  gboolean has_saved;
  int saved;                     /* from 5036 / 5005 or the summary 5037 / 5004 */
  gboolean has_failed;
  int failed;                    /* from the summary 5037 / 5004 */
  GPtrArray *errors;             /* BroRobotMessage *: every message of severity error, in order */
  BroRobotMessage *first_error;  /* the first error other than 5037 / 5004 (usually the cause); nullable */
  BroRobotMessage *space_warning;/* 5038; nullable */
  GPtrArray *read_errors;        /* BroRobotMessage *: errors while reading the disc's data */
  BroBroMessage *drive_mismatch; /* drive.renumbered; nullable */
  char *debug_log;               /* message 1004's file, as a path; nullable */
  char *libre_drive;             /* the LibreDrive detail of message 1011; nullable */
  BroMakemkvNotice *problem;     /* the first notice other than LibreDrive; nullable */
  char *makemkv_version;         /* message 1005; nullable */
  BroBroMessage *stop_reason;    /* space.makemkvWarning or drive.renumbered; nullable */
  /* private */
  gboolean reads_data;
  int expected_index;
  char *expected_device;
} BroRunAccumulator;

/* @reads_data: the run reads the disc's data (a rip or backup): its errors count as read errors.
 * @expected_index (-1: none) and @expected_device: the drive the run uses, to catch renumbered drives. */
BroRunAccumulator *bro_run_accumulator_new (gboolean reads_data, int expected_index, const char *expected_device);
void bro_run_accumulator_free (BroRunAccumulator *accumulator);

void bro_run_accumulator_feed (BroRunAccumulator *accumulator, const BroRobotEvent *event);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroRunAccumulator, bro_run_accumulator_free)

G_END_DECLS
