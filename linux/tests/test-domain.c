/* test-domain.c: Domain against shared/fixtures (one test per module, as on the other platforms). */
#include "bro-test-fixtures.h"

#include "bro-bro-message.h"
#include "bro-job-kind.h"
#include "bro-job-state.h"
#include "bro-outcome.h"
#include "bro-queue.h"
#include "bro-step-kind.h"
#include "bro-step-state.h"

#include <string.h>

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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/messages/catalogue", test_every_code_of_the_catalogue_is_a_case);
  g_test_add_func ("/messages/errors-and-json", test_messages_become_errors_and_json);
  g_test_add_func ("/jobs/enums", test_job_enums_match_the_shared_schema);
  return g_test_run ();
}
