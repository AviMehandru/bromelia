/* test-domain.c: Domain against shared/fixtures (one test per module, as on the other platforms). */
#include "bro-test-fixtures.h"

#include "bro-bro-message.h"
#include "bro-job-kind.h"
#include "bro-job-state.h"
#include "bro-outcome.h"
#include "bro-queue.h"
#include "bro-step-kind.h"
#include "bro-step-state.h"

#include "bro-disc-flags.h"
#include "bro-drive-join.h"
#include "bro-message-catalog.h"
#include "bro-robot.h"
#include "bro-run-accumulator.h"

#include <string.h>

#define J(v, k) bro_json_value_member ((v), (k))
#define JS(v, k) bro_json_value_get_string (J ((v), (k)), NULL)

/* ---- messages ---------------------------------------------------------------------------------------- */

static void
test_every_code_of_the_catalogue_is_a_case (void)
{
  g_autoptr (BroJsonValue) doc = bro_test_fixture_json ("../messages/codes.json");
  BroJsonValue *codes = bro_json_value_member (doc, "codes");
  g_assert_cmpuint (codes->keys->len, ==, BRO_MSG_COUNT_);
  for (guint i = 0; i < codes->keys->len; i++) {
    g_assert_cmpstr (bro_message_code_wire ((BroMessageCode) i), ==, codes->keys->pdata[i]);
    BroMessageCode c;
    g_assert_true (bro_message_code_parse (codes->keys->pdata[i], &c));
    g_assert_cmpint (c, ==, i);
  }
  BroMessageCode c;
  g_assert_false (bro_message_code_parse ("no.suchCode", &c));
  g_assert_cmpstr (bro_message_code_wire (BRO_MSG_RIP_READ_ERRORS), ==, "rip.readErrors");
}

static void
test_messages_become_errors_and_json (void)
{
  BroJsonValue *params = bro_json_value_new_object ();
  bro_json_value_set (params, "library", bro_json_value_new_string ("Films"));
  g_autoptr (BroBroMessage) m = bro_bro_message_new (BRO_MSG_LIBRARY_OFFLINE, params, BRO_SEVERITY_ERROR);
  g_autoptr (BroBroError) e = bro_bro_message_to_error (m, NULL);
  g_assert_cmpstr (e->code, ==, "library.offline");
  g_assert_cmpstr (bro_json_value_get_string (bro_json_value_member (e->params, "library"), NULL), ==, "Films");
  g_autoptr (BroJsonValue) json = bro_bro_message_to_json (m);
  g_autoptr (BroJsonValue) expected = bro_json_value_parse ("{\"code\": \"library.offline\", \"params\": {\"library\": \"Films\"}}", -1);
  g_assert_true (bro_json_value_equal (json, expected));
}

/* ---- jobs -------------------------------------------------------------------------------------------- */

typedef const char *(*ToWire) (int value);
typedef gboolean (*FromWire) (const char *text, int *out);

static void
check_enum (BroJsonValue *defs, const char *name, ToWire to_wire, FromWire from_wire)
{
  BroJsonValue *cases = bro_json_value_member (bro_json_value_member (defs, name), "enum");
  g_assert_cmpuint (bro_json_value_length (cases), >, 0);
  for (guint i = 0; i < bro_json_value_length (cases); i++) {
    const char *wire = bro_json_value_get_string (bro_json_value_at (cases, i), NULL);
    g_assert_cmpstr (to_wire ((int) i), ==, wire);
    int v = -1;
    g_assert_true (from_wire (wire, &v));
    g_assert_cmpint (v, ==, i);
  }
  int v;
  g_assert_false (from_wire ("Nope", &v));
}

static void
test_job_enums_match_the_shared_schema (void)
{
  g_autoptr (BroJsonValue) common = bro_test_fixture_json ("../schema/common.json");
  BroJsonValue *defs = bro_json_value_member (common, "$defs");
  check_enum (defs, "JobKind", (ToWire) bro_job_kind_to_wire, (FromWire) bro_job_kind_from_wire);
  check_enum (defs, "JobState", (ToWire) bro_job_state_to_wire, (FromWire) bro_job_state_from_wire);
  check_enum (defs, "Outcome", (ToWire) bro_outcome_to_wire, (FromWire) bro_outcome_from_wire);
  check_enum (defs, "StepKind", (ToWire) bro_step_kind_to_wire, (FromWire) bro_step_kind_from_wire);
  check_enum (defs, "StepState", (ToWire) bro_step_state_to_wire, (FromWire) bro_step_state_from_wire);
  check_enum (defs, "Queue", (ToWire) bro_queue_to_wire, (FromWire) bro_queue_from_wire);
  check_enum (defs, "Severity", (ToWire) bro_severity_to_wire, (FromWire) bro_severity_from_wire);
}


/* ---- robot ------------------------------------------------------------------------------------------- */

static BroJsonValue *
strv_json (GStrv v)
{
  BroJsonValue *a = bro_json_value_new_array ();
  for (int i = 0; v && v[i]; i++)
    bro_json_value_append (a, bro_json_value_new_string (v[i]));
  return a;
}

static void
put (BroJsonValue *o, const char *k, BroJsonValue *v)
{
  bro_json_value_set (o, k, v);
}

/* An event as the fixtures write it. */
static BroJsonValue *
event_json (const BroRobotEvent *e)
{
  if (!e)
    return bro_json_value_new_null ();
  BroJsonValue *o = bro_json_value_new_object ();
  switch (e->kind) {
  case BRO_ROBOT_EVENT_MESSAGE:
    put (o, "kind", bro_json_value_new_string ("message"));
    put (o, "code", bro_json_value_new_integer (e->message->code));
    put (o, "flags", bro_json_value_new_integer (e->message->flags));
    put (o, "text", bro_json_value_new_string (e->message->text));
    put (o, "format", bro_json_value_new_string (e->message->format));
    put (o, "params", strv_json (e->message->params));
    put (o, "severity", bro_json_value_new_string (bro_message_kind_to_wire (bro_message_catalog_severity (e->message))));
    break;
  case BRO_ROBOT_EVENT_PROGRESS_VALUE:
    put (o, "kind", bro_json_value_new_string ("progressValue"));
    put (o, "current", bro_json_value_new_integer (e->current));
    put (o, "total", bro_json_value_new_integer (e->total));
    put (o, "max", bro_json_value_new_integer (e->max));
    break;
  case BRO_ROBOT_EVENT_PROGRESS_TOTAL:
  case BRO_ROBOT_EVENT_PROGRESS_CURRENT:
    put (o, "kind", bro_json_value_new_string (e->kind == BRO_ROBOT_EVENT_PROGRESS_TOTAL ? "progressTotal" : "progressCurrent"));
    put (o, "code", bro_json_value_new_integer (e->code));
    put (o, "id", bro_json_value_new_integer (e->id));
    put (o, "name", bro_json_value_new_string (e->name));
    break;
  case BRO_ROBOT_EVENT_TITLE_COUNT:
    put (o, "kind", bro_json_value_new_string ("titleCount"));
    put (o, "count", bro_json_value_new_integer (e->count));
    break;
  case BRO_ROBOT_EVENT_RAW:
    put (o, "kind", bro_json_value_new_string ("raw"));
    put (o, "text", bro_json_value_new_string (e->text));
    break;
  default:
    put (o, "kind", bro_json_value_new_integer (e->kind));
  }
  return o;
}

static BroJsonValue *
notice_json (const BroMakemkvNotice *n)
{
  static const char *kinds[] = { "libreDrive", "libreDriveRequired", "keyExpired", "evaluationNotStarted", "versionTooOld" };
  if (!n)
    return bro_json_value_new_null ();
  BroJsonValue *o = bro_json_value_new_object ();
  put (o, "kind", bro_json_value_new_string (kinds[n->kind]));
  if (n->kind == BRO_MAKEMKV_NOTICE_LIBRE_DRIVE)
    put (o, "detail", bro_json_value_new_string (n->detail));
  put (o, "licenseProblem", bro_json_value_new_bool (bro_makemkv_notice_is_license_problem (n)));
  return o;
}

static BroRunAccumulator *
accumulate (const char *fixture)
{
  BroRunAccumulator *acc = bro_run_accumulator_new (FALSE, -1, NULL);
  g_autofree char *text = bro_test_fixture_text (fixture, NULL);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    g_autoptr (BroRobotEvent) e = bro_robot_parse_line (lines[i]);
    if (e)
      bro_run_accumulator_feed (acc, e);
  }
  return acc;
}

static gboolean
robot_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  BroJsonValue *v;
  if ((v = J (given, "fields"))) {
    g_auto (GStrv) f = bro_robot_split_fields (bro_json_value_get_string (v, ""));
    g_autoptr (BroJsonValue) actual = strv_json (f);
    bro_test_same_json (failures, id, "fields", J (expect, "fields"), actual);
    return TRUE;
  }
  if ((v = J (given, "line"))) {
    g_autoptr (BroRobotEvent) e = bro_robot_parse_line (bro_json_value_get_string (v, ""));
    if (J (expect, "event")) {
      g_autoptr (BroJsonValue) actual = event_json (e);
      bro_test_same_json (failures, id, "event", J (expect, "event"), actual);
    } else if (J (expect, "notice")) {
      g_autoptr (BroMakemkvNotice) n = e && e->kind == BRO_ROBOT_EVENT_MESSAGE ? bro_message_catalog_notice (e->message) : NULL;
      g_autoptr (BroJsonValue) actual = notice_json (n);
      bro_test_same_json (failures, id, "notice", J (expect, "notice"), actual);
    } else {
      return FALSE;
    }
    return TRUE;
  }
  if ((v = J (given, "message"))) {
    g_autoptr (BroRobotMessage) m = bro_robot_message_new (bro_json_value_get_integer (J (v, "code"), 0),
                                                          bro_json_value_get_integer (J (v, "flags"), 0), JS (v, "text"));
    bro_test_same_string (failures, id, "severity", JS (expect, "severity"), bro_message_kind_to_wire (bro_message_catalog_severity (m)));
    return TRUE;
  }
  if ((v = J (given, "codes"))) {
    const char *kinds[] = { "error", "warning" };
    for (int k = 0; k < 2; k++) {
      BroJsonValue *codes = J (v, kinds[k]);
      for (guint i = 0; i < bro_json_value_length (codes); i++) {
        g_autoptr (BroRobotMessage) m = bro_robot_message_new (bro_json_value_get_integer (bro_json_value_at (codes, i), 0),
                                                              bro_json_value_get_integer (J (given, "flags"), 0), "x");
        bro_test_same_string (failures, id, "severity", kinds[k], bro_message_kind_to_wire (bro_message_catalog_severity (m)));
      }
    }
    return TRUE;
  }
  if ((v = J (given, "fixture"))) {
    g_autoptr (BroRunAccumulator) acc = accumulate (bro_json_value_get_string (v, ""));
    if (J (expect, "lastSaved")) {
      g_autoptr (BroJsonValue) saved = acc->has_saved ? bro_json_value_new_integer (acc->saved) : bro_json_value_new_null ();
      bro_test_same_json (failures, id, "saved", J (expect, "lastSaved"), saved);
    }
    if (J (expect, "errorCodes")) {
      g_autoptr (BroJsonValue) codes = bro_json_value_new_array ();
      for (guint i = 0; i < acc->errors->len; i++)
        bro_json_value_append (codes, bro_json_value_new_integer (((BroRobotMessage *) acc->errors->pdata[i])->code));
      bro_test_same_json (failures, id, "error codes", J (expect, "errorCodes"), codes);
    }
    if (J (expect, "lastErrorParams")) {
      BroRobotMessage *last = acc->errors->len ? acc->errors->pdata[acc->errors->len - 1] : NULL;
      g_autoptr (BroJsonValue) params = strv_json (last ? last->params : NULL);
      bro_test_same_json (failures, id, "last error's params", J (expect, "lastErrorParams"), params);
    }
    return TRUE;
  }
  if ((v = J (given, "fixtures"))) {
    for (guint i = 0; i < bro_json_value_length (v); i++) {
      const char *f = bro_json_value_get_string (bro_json_value_at (v, i), "");
      const char *part = JS (J (expect, "firstErrorContains"), f);
      g_autoptr (BroRunAccumulator) acc = accumulate (f);
      if (!acc->first_error || !part || !strstr (acc->first_error->text, part))
        bro_test_fail (failures, id, "%s: first error %s doesn't contain %s", f, acc->first_error ? acc->first_error->text : "none", part);
    }
    return TRUE;
  }
  return FALSE;
}

static void
test_robot_cases (void)
{
  bro_test_run_cases ("domain/robot.cases.json", robot_case);
}

static void
test_drive_scan_fixture (void)
{
  g_autoptr (BroJsonValue) golden = bro_test_fixture_json ("domain/drive-scan.drives.expected.json");
  g_autofree char *text = bro_test_fixture_text (JS (golden, "input"), NULL);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  g_autoptr (BroJsonValue) actual = bro_json_value_new_array ();
  for (int i = 0; lines[i]; i++) {
    g_autoptr (BroRobotEvent) e = bro_robot_parse_line (lines[i]);
    g_autoptr (BroMakemkvDrive) d = bro_makemkv_drive_from (e);
    if (!d)
      continue;
    BroJsonValue *o = bro_json_value_new_object ();
    BroJsonValue *flags = bro_json_value_new_array ();
    const char *names[] = { "dvdFiles", "hdDvdFiles", "blurayFiles", "aacsFiles", "bdsvmFiles" };
    for (int b = 0; b < 5; b++)
      if (d->flags.raw & (1 << b))
        bro_json_value_append (flags, bro_json_value_new_string (names[b]));
    put (o, "index", bro_json_value_new_integer (d->index));
    put (o, "state", bro_json_value_new_string (bro_drive_state_to_wire (d->state)));
    put (o, "present", bro_json_value_new_bool (bro_makemkv_drive_is_present (d)));
    put (o, "flags", flags);
    put (o, "typeText", bro_json_value_new_string (bro_disc_flags_type_text (d->flags)));
    put (o, "identification", bro_json_value_new_string (d->identification));
    put (o, "label", bro_json_value_new_string (d->label));
    put (o, "device", bro_json_value_new_string (d->device));
    bro_json_value_append (actual, o);
  }
  g_autoptr (GPtrArray) failures = g_ptr_array_new_with_free_func (g_free);
  bro_test_same_json (failures, "drive-scan", "drives", J (golden, "expect"), actual);
  for (guint i = 0; i < failures->len; i++)
    g_printerr ("%s\n", (char *) failures->pdata[i]);
  g_assert_cmpuint (failures->len, ==, 0);
}

static BroMakemkvDrive *
drive_of (BroJsonValue *d)
{
  return bro_makemkv_drive_new (0, BRO_DRIVE_STATE_INSERTED, 0, JS (d, "identification"), "", JS (d, "device"));
}

static BroDriveMatch *
match_of (BroJsonValue *m)
{
  return bro_drive_match_new (JS (m, "driveName"), JS (m, "devicePath"));
}

static gboolean
only_drive_cases (const char *id)
{
  return g_str_has_prefix (id, "drive-");
}

static gboolean
drive_join_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  if (!only_drive_cases (id))
    return FALSE;
  g_autoptr (BroMakemkvDrive) drive = J (given, "drive") ? drive_of (J (given, "drive")) : NULL;
  if (J (expect, "matches")) {
    g_autoptr (BroDriveMatch) match = match_of (J (given, "match"));
    g_autoptr (BroJsonValue) actual = bro_json_value_new_bool (bro_drive_join_matches (match, drive));
    bro_test_same_json (failures, id, "matches", J (expect, "matches"), actual);
  } else if (J (expect, "entry")) {
    g_autoptr (GPtrArray) drives = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_drive_entry_free);
    BroJsonValue *list = J (given, "drives");
    for (guint i = 0; i < bro_json_value_length (list); i++) {
      BroJsonValue *d = bro_json_value_at (list, i);
      BroDriveEntry *e = bro_drive_entry_new (JS (d, "id"), match_of (J (d, "match")));
      e->enabled = bro_json_value_get_bool (J (d, "enabled"), TRUE);
      g_ptr_array_add (drives, e);
    }
    BroDriveEntry *e = bro_drive_join_entry_for (drives, drive);
    bro_test_same_string (failures, id, "entry", JS (expect, "entry"), e ? e->id : NULL);
  } else if (J (expect, "driveId")) {
    g_autofree char *actual = bro_drive_join_drive_id (JS (given, "identification"), JS (given, "device"));
    bro_test_same_string (failures, id, "drive id", JS (expect, "driveId"), actual);
  } else if (J (expect, "shortModel")) {
    g_autofree char *actual = bro_drive_join_short_model (JS (given, "identification"));
    bro_test_same_string (failures, id, "short model", JS (expect, "shortModel"), actual);
  } else {
    return FALSE;
  }
  return TRUE;
}

static void
test_drive_join_cases (void)
{
  bro_test_run_cases_only ("domain/rules.cases.json", only_drive_cases, drive_join_case);
  bro_test_run_cases_only ("domain/identity.cases.json", only_drive_cases, drive_join_case);
}

static void
test_joins_makemkv_and_os_drives (void)
{
  g_autoptr (GPtrArray) mk = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_makemkv_drive_free);
  g_ptr_array_add (mk, bro_makemkv_drive_new (0, BRO_DRIVE_STATE_INSERTED, 4, "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325", "MOVIE", "/dev/rdisk4"));
  g_ptr_array_add (mk, bro_makemkv_drive_new (1, BRO_DRIVE_STATE_NO_DRIVE, 0, "", "", ""));
  g_autoptr (GPtrArray) os = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_os_drive_free);
  g_ptr_array_add (os, bro_os_drive_new ("/dev/disk5", NULL, NULL));
  g_ptr_array_add (os, bro_os_drive_new ("/dev/disk4", "HL-DT-ST", "/Volumes/MOVIE"));
  g_autoptr (GPtrArray) joined = bro_drive_join_join (mk, os);
  g_assert_cmpuint (joined->len, ==, 2);
  BroJoinedDrive *first = joined->pdata[0], *second = joined->pdata[1];
  g_assert_cmpstr (first->drive_id, ==, "drv-86920bcfd062b640");
  g_assert_cmpstr (first->os->mount_path, ==, "/Volumes/MOVIE");
  g_assert_null (second->makemkv);
  g_autofree char *id = bro_drive_join_drive_id ("", "/dev/disk5");
  g_assert_cmpstr (second->drive_id, ==, id);
}

static void
feed_line (BroRunAccumulator *acc, const char *line)
{
  g_autoptr (BroRobotEvent) e = bro_robot_parse_line (line);
  bro_run_accumulator_feed (acc, e);
}

static void
test_accumulator_stops (void)
{
  g_autoptr (BroRunAccumulator) acc = bro_run_accumulator_new (TRUE, 0, "/dev/rdisk4");
  feed_line (acc, "DRV:0,2,999,12,\"BD-RE X\",\"DISC\",\"/dev/rdisk4\"");
  g_assert_null (acc->stop_reason);
  feed_line (acc, "MSG:1004,0,1,\"Debug logging enabled, log will be saved as file:///Users/me/My%20Logs/MakeMKV_log.txt\",\"%1\",\"file:///Users/me/My%20Logs/MakeMKV_log.txt\"");
  g_assert_cmpstr (acc->debug_log, ==, "/Users/me/My Logs/MakeMKV_log.txt");
  feed_line (acc, "DRV:0,2,999,12,\"BD-RE X\",\"DISC\",\"/dev/rdisk5\"");
  g_assert_nonnull (acc->stop_reason);
  g_assert_cmpint (acc->stop_reason->code, ==, BRO_MSG_DRIVE_RENUMBERED);
  feed_line (acc, "MSG:2003,516,3,\"Error 'Scsi error - MEDIUM ERROR' occurred while reading\",\"x\",\"a\",\"b\",\"c\"");
  g_assert_cmpuint (acc->read_errors->len, ==, 1);
  g_assert_cmpint (acc->first_error->code, ==, 2003);
  g_autoptr (BroRunAccumulator) space = bro_run_accumulator_new (FALSE, -1, NULL);
  feed_line (space, "MSG:5038,0,0,\"The total size of all output files may reach as much as 40 Gb while there are only 10 Gb free\",\"x\"");
  g_assert_cmpint (space->stop_reason->code, ==, BRO_MSG_SPACE_MAKEMKV_WARNING);
  g_autoptr (BroRunAccumulator) windows = bro_run_accumulator_new (FALSE, -1, NULL);
  feed_line (windows, "MSG:1004,0,1,\"x\",\"%1\",\"file:///C:/Users/me/log.txt\"");
  g_assert_cmpstr (windows->debug_log, ==, "C:/Users/me/log.txt");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/messages/catalogue", test_every_code_of_the_catalogue_is_a_case);
  g_test_add_func ("/messages/errors-and-json", test_messages_become_errors_and_json);
  g_test_add_func ("/jobs/enums", test_job_enums_match_the_shared_schema);
  g_test_add_func ("/robot/cases", test_robot_cases);
  g_test_add_func ("/robot/drive-scan", test_drive_scan_fixture);
  g_test_add_func ("/robot/drive-join", test_drive_join_cases);
  g_test_add_func ("/robot/join", test_joins_makemkv_and_os_drives);
  g_test_add_func ("/robot/accumulator", test_accumulator_stops);
  return g_test_run ();
}
