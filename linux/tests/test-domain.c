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
#include "bro-episode-continuation.h"
#include "bro-format-detector.h"
#include "bro-identity.h"
#include "bro-kind-heuristics.h"
#include "bro-label-parser.h"
#include "bro-menu-numbers.h"
#include "bro-job-outcome.h"
#include "bro-run-outcome.h"
#include "bro-test-english.h"
#include "bro-duration.h"
#include "bro-listing-match.h"
#include "bro-one-pass.h"
#include "bro-space-estimate.h"
#include "bro-title-rules.h"
#include "bro-fingerprint.h"
#include "bro-listing-builder.h"
#include "bro-drive-join.h"
#include "bro-message-catalog.h"
#include "bro-robot.h"
#include "bro-run-accumulator.h"

#include <stdlib.h>
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

/* ---- listing and fingerprint ------------------------------------------------------------------------- */

static BroListing *
listing_of (const char *robot_text)
{
  g_autoptr (BroListingBuilder) b = bro_listing_builder_new ();
  g_auto (GStrv) lines = g_strsplit (robot_text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    g_autoptr (BroRobotEvent) e = bro_robot_parse_line (lines[i]);
    if (e)
      bro_listing_builder_feed (b, e);
  }
  return bro_listing_builder_build (b);
}

static int
compare_keys (gconstpointer a, gconstpointer b)
{
  int x = GPOINTER_TO_INT (*(gpointer *) a), y = GPOINTER_TO_INT (*(gpointer *) b);
  return x < y ? -1 : x > y;
}

static BroJsonValue *
attributes_json (GHashTable *a)
{
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init (&it, a);
  while (g_hash_table_iter_next (&it, &k, NULL))
    g_ptr_array_add (keys, k);
  g_ptr_array_sort (keys, compare_keys);
  BroJsonValue *o = bro_json_value_new_object ();
  for (guint i = 0; i < keys->len; i++) {
    g_autofree char *key = g_strdup_printf ("%d", GPOINTER_TO_INT (keys->pdata[i]));
    put (o, key, bro_json_value_new_string (g_hash_table_lookup (a, keys->pdata[i])));
  }
  return o;
}

static BroJsonValue *
int_or_null (gboolean has, int v)
{
  return has ? bro_json_value_new_integer (v) : bro_json_value_new_null ();
}

/* A listing as the golden writes it. */
static BroJsonValue *
listing_json (const BroListing *l)
{
  BroJsonValue *o = bro_json_value_new_object ();
  put (o, "name", bro_json_value_new_string (l->name));
  put (o, "volumeName", bro_json_value_new_string (l->volume_name));
  put (o, "type", bro_json_value_new_string (bro_disc_type_to_wire (l->type)));
  put (o, "typeText", bro_json_value_new_string (l->type_text));
  put (o, "reportedTitleCount", bro_json_value_new_integer (l->reported_title_count));
  BroJsonValue *titles = bro_json_value_new_array ();
  for (guint i = 0; i < l->titles->len; i++) {
    const BroTitle *t = l->titles->pdata[i];
    BroJsonValue *j = bro_json_value_new_object ();
    put (j, "index", bro_json_value_new_integer (t->index));
    put (j, "sourceTitleId", int_or_null (t->has_source_title_id, t->source_title_id));
    put (j, "sourceFile", bro_json_value_new_string (t->source_file));
    put (j, "name", bro_json_value_new_string (t->name));
    put (j, "comment", bro_json_value_new_string (t->comment));
    put (j, "duration", bro_json_value_new_string (t->duration));
    put (j, "durationSeconds", bro_json_value_new_integer (t->duration_seconds));
    put (j, "chapters", bro_json_value_new_integer (t->chapters));
    put (j, "sizeBytes", bro_json_value_new_integer (t->size_bytes));
    put (j, "segmentMap", bro_json_value_new_string (t->segment_map));
    put (j, "outputFileName", bro_json_value_new_string (t->output_file_name));
    put (j, "angle", int_or_null (t->has_angle, t->angle));
    BroJsonValue *tracks = bro_json_value_new_array ();
    for (guint k = 0; k < t->tracks->len; k++) {
      const BroTrack *x = t->tracks->pdata[k];
      BroJsonValue *tj = bro_json_value_new_object ();
      put (tj, "index", bro_json_value_new_integer (x->index));
      put (tj, "kind", bro_json_value_new_string (bro_track_kind_to_wire (x->kind)));
      put (tj, "codec", bro_json_value_new_string (x->codec));
      put (tj, "language", bro_json_value_new_string (x->language));
      put (tj, "languageName", bro_json_value_new_string (x->language_name));
      put (tj, "name", bro_json_value_new_string (x->name));
      put (tj, "isDefault", bro_json_value_new_bool (x->is_default));
      put (tj, "attributes", attributes_json (x->attributes));
      bro_json_value_append (tracks, tj);
    }
    put (j, "tracks", tracks);
    put (j, "attributes", attributes_json (t->attributes));
    bro_json_value_append (titles, j);
  }
  put (o, "titles", titles);
  put (o, "attributes", attributes_json (l->attributes));
  return o;
}

static void
test_listing_of_a_real_dvd (void)
{
  g_autoptr (BroJsonValue) golden = bro_test_fixture_json ("domain/info-dvd.listing.expected.json");
  g_autofree char *text = bro_test_fixture_text (JS (golden, "input"), NULL);
  g_autoptr (BroListing) listing = listing_of (text);
  g_autoptr (BroJsonValue) actual = listing_json (listing);
  g_autoptr (GPtrArray) failures = g_ptr_array_new_with_free_func (g_free);
  bro_test_same_json (failures, "info-dvd", "listing", J (golden, "expect"), actual);
  for (guint i = 0; i < failures->len; i++)
    g_printerr ("%s\n", (char *) failures->pdata[i]);
  g_assert_cmpuint (failures->len, ==, 0);
}

static char *
fingerprint_of_disc (const BroJsonValue *disc)
{
  g_autofree char *text = bro_test_generated_listing (disc);
  g_autoptr (BroListing) listing = listing_of (text);
  return bro_fingerprint_of (listing);
}

static gboolean
fingerprint_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  BroJsonValue *v;
  if ((v = J (given, "listingFile"))) {
    g_autofree char *text = bro_test_fixture_text (bro_json_value_get_string (v, ""), NULL);
    g_autoptr (BroListing) listing = listing_of (text);
    g_autofree char *fp = bro_fingerprint_of (listing);
    bro_test_same_string (failures, id, "fingerprint", JS (expect, "fingerprint"), fp);
  } else if ((v = J (given, "listings"))) {
    g_autoptr (BroJsonValue) prints = bro_json_value_new_array ();
    g_autoptr (GHashTable) distinct = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < bro_json_value_length (v); i++) {
      char *fp = fingerprint_of_disc (bro_json_value_at (v, i));
      bro_json_value_append (prints, fp ? bro_json_value_new_string (fp) : bro_json_value_new_null ());
      g_hash_table_add (distinct, fp ? fp : g_strdup (""));
    }
    bro_test_same_json (failures, id, "fingerprints", J (expect, "fingerprints"), prints);
    g_autoptr (BroJsonValue) equal = bro_json_value_new_bool (g_hash_table_size (distinct) == 1);
    bro_test_same_json (failures, id, "equal", J (expect, "equal"), equal);
  } else if ((v = J (given, "listing"))) {
    g_autofree char *fp = fingerprint_of_disc (v);
    bro_test_same_string (failures, id, "fingerprint", JS (expect, "fingerprint"), fp);
  } else {
    return FALSE;
  }
  return TRUE;
}

static void
test_fingerprint_cases (void)
{
  bro_test_run_cases ("domain/fingerprint.cases.json", fingerprint_case);
}

/* ---- titles ------------------------------------------------------------------------------------------ */

/* A title written in a fixture: any of the Title fields, the rest empty. */
static BroTitle *
title_of (BroJsonValue *t)
{
  BroTitle *x = g_new0 (BroTitle, 1);
  x->index = bro_json_value_get_integer (J (t, "index"), 0);
  x->has_source_title_id = J (t, "sourceTitleId") != NULL;
  x->source_title_id = bro_json_value_get_integer (J (t, "sourceTitleId"), 0);
  x->source_file = g_strdup (bro_json_value_get_string (J (t, "sourceFile"), ""));
  x->name = g_strdup (bro_json_value_get_string (J (t, "name"), ""));
  x->comment = g_strdup (bro_json_value_get_string (J (t, "comment"), ""));
  x->duration = g_strdup (bro_json_value_get_string (J (t, "duration"), ""));
  BroDuration d;
  x->duration_seconds = J (t, "durationSeconds") ? bro_json_value_get_integer (J (t, "durationSeconds"), 0)
                        : bro_duration_parse_clock (x->duration, &d) ? (int) d.seconds : 0;
  x->chapters = bro_json_value_get_integer (J (t, "chapters"), 0);
  x->size_bytes = bro_json_value_get_integer (J (t, "sizeBytes"), 0);
  x->segment_map = g_strdup (bro_json_value_get_string (J (t, "segmentMap"), ""));
  x->output_file_name = g_strdup (bro_json_value_get_string (J (t, "outputFileName"), ""));
  x->has_angle = J (t, "angle") != NULL;
  x->angle = bro_json_value_get_integer (J (t, "angle"), 0);
  x->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_track_free);
  x->attributes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  return x;
}

static BroListing *
listing_of_titles (BroJsonValue *titles)
{
  BroListing *l = g_atomic_rc_box_new0 (BroListing);
  l->name = g_strdup ("");
  l->volume_name = g_strdup ("");
  l->type_text = g_strdup ("");
  l->type = BRO_DISC_TYPE_DISC;
  l->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_title_free);
  for (guint i = 0; i < bro_json_value_length (titles); i++)
    g_ptr_array_add (l->titles, title_of (bro_json_value_at (titles, i)));
  l->attributes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  return l;
}

static BroTitleSettings *
settings_of (BroJsonValue *r)
{
  BroTitleSettings *s = bro_title_settings_new ();
  BroJsonValue *v;
  if ((v = J (r, "strategy")))
    bro_title_strategy_from_wire (bro_json_value_get_string (v, ""), &s->strategy);
  s->longest_count = bro_json_value_get_integer (J (r, "longestCount"), 1);
  if ((v = J (r, "indexPattern"))) {
    g_free (s->index_pattern);
    s->index_pattern = g_strdup (bro_json_value_get_string (v, ""));
  }
  if ((v = J (r, "indexBase")))
    bro_index_base_from_wire (bro_json_value_get_string (v, ""), &s->index_base);
  s->min_duration_seconds = bro_json_value_get_integer (J (r, "minDurationSeconds"), 0);
  s->max_duration_seconds = bro_json_value_get_integer (J (r, "maxDurationSeconds"), 0);
  s->min_chapters = bro_json_value_get_integer (J (r, "minChapters"), 0);
  s->max_chapters = bro_json_value_get_integer (J (r, "maxChapters"), 0);
  s->min_size_mb = bro_json_value_get_integer (J (r, "minSizeMB"), 0);
  s->max_size_mb = bro_json_value_get_integer (J (r, "maxSizeMB"), 0);
  if ((v = J (r, "includePattern"))) {
    g_free (s->include_pattern);
    s->include_pattern = g_strdup (bro_json_value_get_string (v, ""));
  }
  if ((v = J (r, "excludePattern"))) {
    g_free (s->exclude_pattern);
    s->exclude_pattern = g_strdup (bro_json_value_get_string (v, ""));
  }
  s->skip_duplicates = bro_json_value_get_bool (J (r, "skipDuplicates"), TRUE);
  s->skip_alternate_angles = bro_json_value_get_bool (J (r, "skipAlternateAngles"), FALSE);
  s->max_titles = bro_json_value_get_integer (J (r, "maxTitles"), 0);
  return s;
}

static GArray *
ints_of (BroJsonValue *v)
{
  GArray *a = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < bro_json_value_length (v); i++) {
    int n = bro_json_value_get_integer (bro_json_value_at (v, i), 0);
    g_array_append_val (a, n);
  }
  return a;
}

static BroJsonValue *titles_inputs;

static gboolean
title_rules_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroListing) listing = NULL;
  if (J (given, "titles")) {
    listing = listing_of_titles (J (titles_inputs, JS (given, "titles")));
  } else {
    g_autofree char *text = bro_test_fixture_text (JS (given, "listing"), NULL);
    listing = listing_of (text);
  }
  g_autoptr (BroTitleSettings) rules = settings_of (J (given, "rules"));
  g_autoptr (BroSelection) s = bro_title_rules_select (listing, rules);
  g_autoptr (BroJsonValue) selected = bro_json_value_new_array ();
  for (guint i = 0; i < s->indices->len; i++)
    bro_json_value_append (selected, bro_json_value_new_integer (g_array_index (s->indices, int, i)));
  bro_test_same_json (failures, id, "selected", J (expect, "selected"), selected);
  g_autoptr (BroJsonValue) manual = bro_json_value_new_bool (s->requires_manual_choice);
  g_autoptr (BroJsonValue) no = bro_json_value_new_bool (FALSE);
  bro_test_same_json (failures, id, "requires manual choice", J (expect, "requiresManualChoice") ? J (expect, "requiresManualChoice") : no, manual);
  bro_test_same_string (failures, id, "error", JS (J (expect, "error"), "code"), s->error ? bro_message_code_wire (s->error->code) : NULL);
  BroJsonValue *reasons = J (expect, "reasons");
  for (guint i = 0; reasons && i < reasons->keys->len; i++) {
    int index = atoi (reasons->keys->pdata[i]);
    for (guint k = 0; k < s->trace->len; k++) {
      BroSelectionTrace *t = s->trace->pdata[k];
      if (t->index != index)
        continue;
      g_autoptr (BroJsonValue) r = bro_bro_message_to_json (t->reason);
      bro_test_same_json (failures, id, "reason", reasons->items->pdata[i], r);
    }
  }
  if (s->trace->len != listing->titles->len)
    bro_test_fail (failures, id, "trace has %u entries for %u titles", s->trace->len, listing->titles->len);
  return TRUE;
}

static void
test_title_rules_cases (void)
{
  g_autoptr (BroJsonValue) doc = bro_test_fixture_json ("domain/titles.cases.json");
  titles_inputs = J (doc, "inputs");
  bro_test_run_cases ("domain/titles.cases.json", title_rules_case);
  titles_inputs = NULL;
}

static gboolean
one_pass_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  if (J (given, "titles") && J (given, "chosen")) {
    g_autoptr (BroListing) listing = listing_of_titles (J (given, "titles"));
    g_autoptr (GArray) chosen = ints_of (J (given, "chosen"));
    BroOnePassPlan plan;
    BroJsonValue *current = J (given, "current");
    int cur = current && current->kind != BRO_JSON_VALUE_NULL ? bro_json_value_get_integer (current, 0) : -1;
    g_autoptr (BroJsonValue) actual = bro_one_pass_plan (chosen, listing, cur, &plan) ? bro_json_value_new_integer (plan.min_length)
                                                                                         : bro_json_value_new_null ();
    bro_test_same_json (failures, id, "min length", J (expect, "minLength"), actual);
  } else if (J (given, "listing")) {
    g_autoptr (BroListing) listing = listing_of_titles (J (given, "listing"));
    g_autoptr (BroListing) original = listing_of_titles (J (given, "of"));
    g_autoptr (GArray) chosen = ints_of (J (given, "chosen"));
    g_autoptr (BroJsonValue) actual = bro_json_value_new_bool (bro_one_pass_matches (listing, chosen, original));
    bro_test_same_json (failures, id, "matches", J (expect, "matches"), actual);
  } else if (J (given, "bytes")) {
    BroBytes b = { bro_json_value_get_integer (J (given, "bytes"), 0) };
    g_autoptr (BroJsonValue) actual = bro_json_value_new_integer (bro_space_estimate_required (b).count);
    bro_test_same_json (failures, id, "required", J (expect, "required"), actual);
  } else if (J (given, "titles")) {
    g_autoptr (BroListing) listing = listing_of_titles (J (given, "titles"));
    g_autoptr (GArray) hand = ints_of (J (given, "handPicked"));
    BroJsonValue *split = J (given, "splitTitle");
    BroBytes need = bro_space_estimate_for_plan (listing->titles, hand, split ? bro_json_value_get_integer (split, -1) : -1);
    g_autoptr (BroJsonValue) bytes = bro_json_value_new_integer (need.count);
    g_autoptr (BroJsonValue) required = bro_json_value_new_integer (bro_space_estimate_required (need).count);
    bro_test_same_json (failures, id, "bytes", J (expect, "bytes"), bytes);
    bro_test_same_json (failures, id, "required", J (expect, "required"), required);
  } else {
    return FALSE;
  }
  return TRUE;
}

static void
test_one_pass_and_space_cases (void)
{
  bro_test_run_cases ("domain/one-pass-and-space.cases.json", one_pass_case);
}

static BroListing *
small_listing (const char *volume, int n, const int (*titles)[3])
{
  g_autoptr (BroJsonValue) array = bro_json_value_new_array ();
  for (int i = 0; i < n; i++) {
    BroJsonValue *t = bro_json_value_new_object ();
    put (t, "index", bro_json_value_new_integer (i));
    put (t, "sourceTitleId", bro_json_value_new_integer (titles[i][0]));
    put (t, "durationSeconds", bro_json_value_new_integer (titles[i][1]));
    g_autofree char *map = g_strdup_printf ("%d", titles[i][2]);
    put (t, "segmentMap", bro_json_value_new_string (map));
    bro_json_value_append (array, t);
  }
  BroListing *l = listing_of_titles (array);
  g_free (l->volume_name);
  l->volume_name = g_strdup (volume);
  return l;
}

static void
test_listing_match (void)
{
  const int opened_titles[][3] = { { 1, 1300, 1 }, { 2, 1310, 2 }, { 3, 1320, 3 } };
  const int renumbered_titles[][3] = { { 2, 1310, 2 }, { 3, 1320, 3 } };
  const int other_titles[][3] = { { 9, 99, 9 } };
  g_autoptr (BroListing) opened = small_listing ("SHOW", 3, opened_titles);
  g_autoptr (BroListing) renumbered = small_listing ("SHOW", 2, renumbered_titles);
  g_autoptr (BroListing) other_volume = small_listing ("OTHER", 1, opened_titles);
  g_autoptr (BroListing) other_titles_listing = small_listing ("SHOW", 1, other_titles);
  g_assert_null (bro_listing_match_different_disc (opened, renumbered));
  g_autoptr (GArray) two = g_array_new (FALSE, FALSE, sizeof (int));
  int one_two[] = { 1, 2 };
  g_array_append_vals (two, one_two, 2);
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GHashTable) map = bro_listing_match_map_titles (two, opened, renumbered, NULL, &error);
  g_assert_nonnull (map);
  g_assert_cmpint (GPOINTER_TO_INT (g_hash_table_lookup (map, GINT_TO_POINTER (1))), ==, 0);
  g_assert_cmpint (GPOINTER_TO_INT (g_hash_table_lookup (map, GINT_TO_POINTER (2))), ==, 1);
  g_autoptr (GArray) zero = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_size (zero, 1);
  g_assert_null (bro_listing_match_map_titles (zero, opened, renumbered, NULL, &error));
  g_assert_cmpstr (error->code, ==, "disc.titleGone");
  g_autoptr (BroBroMessage) v = bro_listing_match_different_disc (opened, other_volume);
  g_assert_cmpint (v->code, ==, BRO_MSG_DISC_CHANGED_VOLUME);
  g_autoptr (BroBroMessage) t = bro_listing_match_different_disc (opened, other_titles_listing);
  g_assert_cmpint (t->code, ==, BRO_MSG_DISC_CHANGED_TITLES);
}

/* ---- outcome ----------------------------------------------------------------------------------------- */

static BroJsonValue *
error_json (const BroBroError *e)
{
  BroJsonValue *o = bro_json_value_new_object ();
  put (o, "code", bro_json_value_new_string (e->code));
  if (bro_json_value_length (e->params) > 0)
    put (o, "params", bro_json_value_ref (e->params));
  if (e->cause)
    put (o, "cause", error_json (e->cause));
  return o;
}

static BroStepResult *
step_of (BroJsonValue *s)
{
  BroStepKind kind = 0;
  BroStepState state = 0;
  bro_step_kind_from_wire (JS (s, "kind"), &kind);
  bro_step_state_from_wire (JS (s, "state"), &state);
  BroStepResult *r = bro_step_result_new (kind, state);
  BroJsonValue *e = J (s, "error");
  if (e)
    r->error = bro_bro_error_new (JS (e, "code"), J (e, "params") ? bro_json_value_ref (J (e, "params")) : NULL, NULL);
  r->read_errors = bro_json_value_get_integer (J (s, "readErrors"), 0);
  r->quarantined = bro_json_value_get_bool (J (s, "quarantined"), FALSE);
  const char *notice = JS (s, "notice");
  if (notice) {
    BroMakemkvNoticeKind k = strcmp (notice, "keyExpired") == 0 ? BRO_MAKEMKV_NOTICE_KEY_EXPIRED
                             : strcmp (notice, "evaluationNotStarted") == 0 ? BRO_MAKEMKV_NOTICE_EVALUATION_NOT_STARTED
                             : strcmp (notice, "versionTooOld") == 0 ? BRO_MAKEMKV_NOTICE_VERSION_TOO_OLD
                                                                     : BRO_MAKEMKV_NOTICE_LIBRE_DRIVE_REQUIRED;
    r->notice = bro_makemkv_notice_new (k, NULL);
  }
  BroJsonValue *skip = J (s, "skip");
  if (skip) {
    BroMessageCode code = 0;
    bro_message_code_parse (JS (skip, "code"), &code);
    r->skip = bro_bro_message_new (code, J (skip, "params") ? bro_json_value_ref (J (skip, "params")) : NULL, BRO_SEVERITY_INFO);
  }
  r->affects_outcome = bro_json_value_get_bool (J (s, "affectsOutcome"), TRUE);
  return r;
}

static BroRunAccumulator *
accumulate_data (const char *fixture)
{
  BroRunAccumulator *acc = bro_run_accumulator_new (TRUE, -1, NULL);
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
outcome_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  if (J (given, "fixture")) {
    g_autoptr (BroRunAccumulator) acc = accumulate_data (JS (given, "fixture"));
    BroProcessExit exit = bro_process_exit_status (bro_json_value_get_integer (J (given, "exitCode"), 0));
    g_autoptr (BroRunOutcome) run = bro_run_outcome_classify (acc, &exit, bro_json_value_get_integer (J (given, "producedFiles"), 0));
    bro_test_same_string (failures, id, "status word", JS (expect, "statusWord"), bro_status_word_to_wire (run->status));
    g_autoptr (BroJsonValue) failed = bro_json_value_new_bool (run->status == BRO_STATUS_WORD_FAILED);
    g_autoptr (BroJsonValue) read_errors = bro_json_value_new_bool (run->status == BRO_STATUS_WORD_ERRORS);
    bro_test_same_json (failures, id, "failed", J (expect, "failed"), failed);
    bro_test_same_json (failures, id, "read errors", J (expect, "readErrors"), read_errors);
    g_autoptr (BroJsonValue) error = run->error ? bro_bro_message_to_json (run->error) : NULL;
    g_autofree char *text = error ? bro_test_english (error) : NULL;
    const char *part = JS (expect, "errorContains");
    if (part ? !text || !strstr (text, part) : text != NULL)
      bro_test_fail (failures, id, "error \"%s\" should contain \"%s\"", text ? text : "none", part ? part : "(no error)");
    return TRUE;
  }
  if (J (given, "steps")) {
    g_autoptr (GPtrArray) steps = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_step_result_free);
    BroJsonValue *list = J (given, "steps");
    for (guint i = 0; i < bro_json_value_length (list); i++)
      g_ptr_array_add (steps, step_of (bro_json_value_at (list, i)));
    BroOutcomePolicy policy = { 0 };
    g_autoptr (BroDecided) d = bro_job_outcome_decide (steps, bro_json_value_get_bool (J (given, "cancelled"), FALSE), &policy);
    bro_test_same_string (failures, id, "outcome", JS (expect, "outcome"), bro_outcome_to_wire (d->outcome));
    if (J (expect, "error")) {
      g_autoptr (BroJsonValue) actual = d->error ? error_json (d->error) : bro_json_value_new_null ();
      bro_test_same_json (failures, id, "error", J (expect, "error"), actual);
    } else if (d->outcome == BRO_OUTCOME_SUCCEEDED && d->error) {
      bro_test_fail (failures, id, "a success with an error");
    }
    return TRUE;
  }
  return FALSE;
}

static void
test_outcome_cases (void)
{
  bro_test_run_cases ("domain/outcome.cases.json", outcome_case);
}

static void
test_stalled_and_cancelled_runs (void)
{
  g_autoptr (BroRunAccumulator) acc = bro_run_accumulator_new (TRUE, -1, NULL);
  BroProcessExit stalled = { -1, 9, 600, TRUE, FALSE };
  g_autoptr (BroRunOutcome) run = bro_run_outcome_classify (acc, &stalled, 0);
  g_assert_cmpint (run->status, ==, BRO_STATUS_WORD_FAILED);
  g_autoptr (BroJsonValue) error = bro_bro_message_to_json (run->error);
  g_autofree char *text = bro_test_english (error);
  g_assert_cmpstr (text, ==, "makemkvcon printed nothing for 10 minutes and was stopped (it did not exit; the drive may need to be reset). The drive or disc may be stuck: eject the disc and retry.");
  BroProcessExit cancelled = { 143, 15, -1, FALSE, TRUE };
  g_autoptr (BroRunOutcome) c = bro_run_outcome_classify (acc, &cancelled, 1);
  g_assert_cmpint (c->status, ==, BRO_STATUS_WORD_CANCELLED);
}

/* ---- identity ---------------------------------------------------------------------------------------- */

static GHashTable *
attrs_of (BroJsonValue *a)
{
  GHashTable *t = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  for (guint i = 0; a && a->keys && i < a->keys->len; i++)
    g_hash_table_insert (t, GINT_TO_POINTER (atoi (a->keys->pdata[i])), g_strdup (bro_json_value_get_string (a->items->pdata[i], "")));
  return t;
}

/* A listing written in a fixture: a file name, or {attributes, titles} with tracks. */
static BroListing *
listing_from (BroJsonValue *v)
{
  if (v->kind == BRO_JSON_VALUE_STRING) {
    g_autofree char *text = bro_test_fixture_text (v->string, NULL);
    return listing_of (text);
  }
  BroListing *l = listing_of_titles (J (v, "titles"));
  g_hash_table_unref (l->attributes);
  l->attributes = attrs_of (J (v, "attributes"));
  const char *a1 = g_hash_table_lookup (l->attributes, GINT_TO_POINTER (1));
  const char *a2 = g_hash_table_lookup (l->attributes, GINT_TO_POINTER (2));
  const char *a32 = g_hash_table_lookup (l->attributes, GINT_TO_POINTER (32));
  g_free (l->type_text);
  l->type_text = g_strdup (a1 ? a1 : "");
  g_free (l->name);
  l->name = g_strdup (a2 ? a2 : a32 ? a32 : "");
  g_free (l->volume_name);
  l->volume_name = g_strdup (a32 ? a32 : "");
  BroJsonValue *titles = J (v, "titles");
  for (guint i = 0; i < bro_json_value_length (titles); i++) {
    BroTitle *t = l->titles->pdata[i];
    BroJsonValue *tracks = J (bro_json_value_at (titles, i), "tracks");
    for (guint k = 0; k < bro_json_value_length (tracks); k++) {
      BroJsonValue *tj = bro_json_value_at (tracks, k);
      BroTrack *tr = g_new0 (BroTrack, 1);
      tr->index = bro_json_value_get_integer (J (tj, "index"), 0);
      tr->kind = BRO_TRACK_KIND_UNKNOWN;
      bro_track_kind_from_wire (JS (tj, "kind"), &tr->kind);
      tr->codec = g_strdup ("");
      tr->language = g_strdup ("");
      tr->language_name = g_strdup ("");
      tr->name = g_strdup ("");
      tr->attributes = attrs_of (J (tj, "attributes"));
      g_ptr_array_add (t->tracks, tr);
    }
  }
  return l;
}

static BroJsonValue *
opt_int (int v)
{
  return v >= 0 ? bro_json_value_new_integer (v) : bro_json_value_new_null ();
}

static BroJsonValue *
label_json (const BroLabel *l)
{
  BroJsonValue *o = bro_json_value_new_object ();
  put (o, "title", bro_json_value_new_string (l->title));
  put (o, "season", opt_int (l->season));
  put (o, "part", opt_int (l->part));
  put (o, "volume", opt_int (l->volume));
  put (o, "disc", opt_int (l->disc));
  put (o, "looksLikeSeries", bro_json_value_new_bool (l->looks_like_series));
  return o;
}

static void
check_decision (GPtrArray *failures, const char *id, BroJsonValue *expect, const BroDecision *d)
{
  bro_test_same_string (failures, id, "kind", JS (expect, "kind"), bro_media_kind_to_wire (d->value));
  g_autoptr (BroJsonValue) reason = bro_bro_message_to_json (d->reason);
  bro_test_same_json (failures, id, "reason", J (expect, "reason"), reason);
}

static gboolean
not_drive_case (const char *id)
{
  return !g_str_has_prefix (id, "drive-");
}

static gboolean
identity_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  BroJsonValue *v;
  if ((v = J (given, "label"))) {
    g_autoptr (BroLabel) parsed = bro_label_parser_parse (bro_json_value_get_string (v, ""));
    if (J (given, "playAllEpisodes")) {
      g_autoptr (BroDecision) d = bro_kind_heuristics_decide (parsed, NULL, bro_json_value_get_integer (J (given, "playAllEpisodes"), 0));
      check_decision (failures, id, expect, d);
    } else if (J (expect, "set")) {
      g_autofree char *set = bro_label_parser_set_description (parsed);
      bro_test_same_string (failures, id, "set", JS (expect, "set"), set);
    } else {
      g_autoptr (BroJsonValue) actual = label_json (parsed);
      bro_test_same_json (failures, id, "label", expect, actual);
    }
    return TRUE;
  }
  if ((v = J (given, "listing"))) {
    g_autoptr (BroListing) listing = listing_from (v);
    if (J (expect, "episodeLike")) {
      g_autoptr (GPtrArray) like = bro_kind_heuristics_episode_like (listing->titles);
      g_autoptr (BroJsonValue) indices = bro_json_value_new_array ();
      for (guint i = 0; i < like->len; i++)
        bro_json_value_append (indices, bro_json_value_new_integer (((BroTitle *) like->pdata[i])->index));
      bro_test_same_json (failures, id, "episode-like", J (expect, "episodeLike"), indices);
      g_autoptr (BroLabel) label = bro_label_parser_parse (listing->volume_name);
      g_autoptr (BroDecision) d = bro_kind_heuristics_decide (label, listing, 0);
      check_decision (failures, id, expect, d);
    } else if (!J (given, "encrypted")) {
      bro_test_same_string (failures, id, "format", JS (expect, "format"),
                            bro_disc_format_to_wire (bro_format_detector_detect (listing, NULL, NULL, FALSE)));
    } else {
      g_autoptr (BroIdentityInputs) in = bro_identity_inputs_new (listing, "", bro_json_value_get_bool (J (given, "encrypted"), FALSE));
      if (JS (given, "nameOverride")) {
        g_free (in->name_override);
        in->name_override = g_strdup (JS (given, "nameOverride"));
      }
      if (JS (given, "kindOverride"))
        in->has_kind_override = bro_media_kind_from_wire (JS (given, "kindOverride"), &in->kind_override);
      g_autoptr (BroIdentity) identity = bro_identity_resolve (in);
      g_autofree char *set = bro_label_parser_set_description (identity->label);
      g_autoptr (BroJsonValue) actual = bro_json_value_new_object ();
      put (actual, "name", bro_json_value_new_string (identity->name));
      put (actual, "kind", bro_json_value_new_string (bro_media_kind_to_wire (identity->kind)));
      put (actual, "format", bro_json_value_new_string (bro_disc_format_to_wire (identity->format)));
      put (actual, "formatCode", bro_json_value_new_string (identity->format_code.text));
      put (actual, "set", bro_json_value_new_string (set));
      put (actual, "reason", bro_bro_message_to_json (identity->reason));
      put (actual, "label", label_json (identity->label));
      for (guint i = 0; i < expect->keys->len; i++) {
        const char *key = expect->keys->pdata[i];
        BroJsonValue *want = expect->items->pdata[i];
        if (strcmp (key, "label") == 0) {
          for (guint k = 0; k < want->keys->len; k++)
            bro_test_same_json (failures, id, want->keys->pdata[k], want->items->pdata[k], J (J (actual, "label"), want->keys->pdata[k]));
        } else {
          bro_test_same_json (failures, id, key, want, J (actual, key));
        }
      }
    }
    return TRUE;
  }
  if ((v = J (given, "format"))) {
    BroDiscFormat f = BRO_DISC_FORMAT_UNKNOWN;
    bro_disc_format_from_wire (bro_json_value_get_string (v, ""), &f);
    BroFormatCode code = bro_format_detector_code (f, bro_json_value_get_bool (J (given, "encrypted"), FALSE));
    bro_test_same_string (failures, id, "format code", JS (expect, "formatCode"), code.text);
    return TRUE;
  }
  if ((v = J (given, "numbers"))) {
    g_autoptr (GArray) numbers = ints_of (v);
    int first;
    g_autoptr (BroJsonValue) actual = bro_menu_numbers_first_episode (numbers, bro_json_value_get_integer (J (given, "count"), 0), &first)
                                          ? bro_json_value_new_integer (first) : bro_json_value_new_null ();
    bro_test_same_json (failures, id, "first episode", J (expect, "firstEpisode"), actual);
    return TRUE;
  }
  if ((v = J (given, "text"))) {
    g_autoptr (GArray) numbers = bro_menu_numbers_parse (bro_json_value_get_string (v, ""));
    g_autoptr (BroJsonValue) actual = bro_json_value_new_array ();
    for (guint i = 0; i < numbers->len; i++)
      bro_json_value_append (actual, bro_json_value_new_integer (g_array_index (numbers, int, i)));
    bro_test_same_json (failures, id, "numbers", J (expect, "numbers"), actual);
    return TRUE;
  }
  if (J (expect, "format")) {
    BroDiscFlags flags = { 0 };
    BroJsonValue *f = J (given, "flags");
    const char *names[] = { "dvdFiles", "hdDvdFiles", "blurayFiles", "aacsFiles", "bdsvmFiles" };
    for (guint i = 0; i < bro_json_value_length (f); i++)
      for (int b = 0; b < 5; b++)
        if (g_strcmp0 (bro_json_value_get_string (bro_json_value_at (f, i), ""), names[b]) == 0)
          flags.raw |= 1 << b;
    BroDiscFormat detected = bro_format_detector_detect (NULL, f ? &flags : NULL, JS (given, "indexBdmv"),
                                                         bro_json_value_get_bool (J (given, "backupHasVideoTs"), FALSE));
    bro_test_same_string (failures, id, "format", JS (expect, "format"), bro_disc_format_to_wire (detected));
    return TRUE;
  }
  return FALSE;
}

static void
test_identity_cases (void)
{
  bro_test_run_cases_only ("domain/identity.cases.json", not_drive_case, identity_case);
}

static char *
folder_of (const char *path)
{
  return g_strndup (path, strrchr (path, '/') - path);
}

/* shared/fixtures/episode-continuation.json: the records a case can see are the library's (the output root) and
 * the history's folders; the season folder's file names give its highest episode. */
static void
test_episode_continuation_cases (void)
{
  g_autoptr (BroJsonValue) doc = bro_test_fixture_json ("episode-continuation.json");
  g_autoptr (GPtrArray) records = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_archived_disc_free);
  BroJsonValue *list = J (doc, "records");
  for (guint i = 0; i < bro_json_value_length (list); i++) {
    BroJsonValue *r = bro_json_value_at (list, i), *rec = J (r, "record"), *disc = J (rec, "disc");
    const char *status = JS (rec, "status");
    if (g_strcmp0 (JS (rec, "kind"), "tv") != 0 || (g_strcmp0 (status, "success") != 0 && g_strcmp0 (status, "errors") != 0))
      continue;
    const char *volume = JS (disc, "volumeName");
    g_autoptr (BroLabel) label = bro_label_parser_parse (volume && volume[0] ? volume : JS (disc, "label"));
    g_autofree char *folder = folder_of (JS (r, "path"));
    BroArchivedDisc *d = bro_archived_disc_new (JS (rec, "name"), label->title, folder);
    d->season = bro_json_value_get_integer (J (disc, "season"), -1);
    d->part = bro_json_value_get_integer (J (disc, "part"), -1);
    d->volume = bro_json_value_get_integer (J (disc, "volume"), -1);
    d->disc = bro_json_value_get_integer (J (disc, "disc"), -1);
    BroJsonValue *eps = J (rec, "episodes");
    for (guint k = 0; k < bro_json_value_length (eps); k++)
      d->last_episode = MAX (d->last_episode, (int) bro_json_value_get_integer (J (bro_json_value_at (eps, k), "episode"), -1));
    g_ptr_array_add (records, d);
  }
  BroJsonValue *files = J (doc, "files"), *cases = J (doc, "cases");
  for (guint i = 0; i < bro_json_value_length (cases); i++) {
    BroJsonValue *c = bro_json_value_at (cases, i), *q = J (c, "query"), *folders = J (c, "folders");
    g_autoptr (GPtrArray) visible = g_ptr_array_new ();
    for (guint k = 0; k < records->len; k++) {
      BroArchivedDisc *d = records->pdata[k];
      gboolean seen = g_str_has_prefix (d->folder, "library/");
      for (guint f = 0; !seen && f < bro_json_value_length (folders); f++)
        seen = g_strcmp0 (bro_json_value_get_string (bro_json_value_at (folders, f), ""), d->folder) == 0;
      if (seen)
        g_ptr_array_add (visible, d);
    }
    g_autoptr (BroContinuationQuery) query = bro_continuation_query_new (JS (q, "name"), JS (q, "labelTitle"),
                                                                         bro_json_value_get_integer (J (q, "disc"), 0));
    query->season = bro_json_value_get_integer (J (q, "season"), -1);
    query->part = bro_json_value_get_integer (J (q, "part"), -1);
    query->volume = bro_json_value_get_integer (J (q, "volume"), -1);
    const char *season_folder = JS (c, "seasonFolder");
    if (season_folder) {
      query->season_folder = g_strdup (season_folder);
      g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
      for (guint f = 0; f < bro_json_value_length (files); f++) {
        const char *path = bro_json_value_get_string (bro_json_value_at (files, f), "");
        g_autofree char *folder = folder_of (path);
        if (strcmp (folder, season_folder) == 0)
          g_ptr_array_add (names, g_strdup (strrchr (path, '/') + 1));
      }
      g_ptr_array_add (names, NULL);
      int highest;
      if (bro_episode_continuation_highest_in_season ((const char *const *) names->pdata, bro_json_value_get_integer (J (c, "season"), 0), &highest))
        query->season_folder_highest = highest;
    }
    g_autoptr (BroPreviousEpisode) found = bro_episode_continuation_choose (query, visible);
    gint64 expected = bro_json_value_get_integer (J (c, "expect"), -1);
    if ((found ? found->last_episode : -1) != expected)
      g_error ("%s: expected %" G_GINT64_FORMAT ", got %d", JS (c, "what"), expected, found ? found->last_episode : -1);
  }
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
  g_test_add_func ("/listing/real-dvd", test_listing_of_a_real_dvd);
  g_test_add_func ("/fingerprint/cases", test_fingerprint_cases);
  g_test_add_func ("/titles/rules", test_title_rules_cases);
  g_test_add_func ("/titles/one-pass-and-space", test_one_pass_and_space_cases);
  g_test_add_func ("/titles/listing-match", test_listing_match);
  g_test_add_func ("/outcome/cases", test_outcome_cases);
  g_test_add_func ("/outcome/stalled-and-cancelled", test_stalled_and_cancelled_runs);
  g_test_add_func ("/identity/cases", test_identity_cases);
  g_test_add_func ("/identity/episode-continuation", test_episode_continuation_cases);
  return g_test_run ();
}
