/* test-foundation.c: Foundation against shared/fixtures/foundation. */
#include "bro-bro-error.h"
#include "bro-cancellation-token.h"
#include "bro-duration.h"
#include "bro-id.h"
#include "bro-instant.h"
#include "bro-json-value.h"
#include "bro-test-fixtures.h"

#include <string.h>

static gboolean
foundation_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  BroJsonValue *v;
  if ((v = bro_json_value_member (given, "id"))) {
    BroId bid;
    g_strlcpy (bid.value, bro_json_value_get_string (v, ""), sizeof bid.value);
    g_autofree char *s = bro_id_short (&bid);
    bro_test_same_string (failures, id, "short id", bro_json_value_get_string (bro_json_value_member (expect, "short"), NULL), s);
    return TRUE;
  }
  if ((v = bro_json_value_member (given, "instant"))) {
    BroInstant t;
    g_autofree char *s = bro_instant_parse (bro_json_value_get_string (v, ""), &t) ? bro_instant_format (t) : NULL;
    bro_test_same_string (failures, id, "instant", bro_json_value_get_string (bro_json_value_member (expect, "formatted"), NULL), s);
    return TRUE;
  }
  if ((v = bro_json_value_member (given, "clock"))) {
    BroDuration d;
    gboolean ok = bro_duration_parse_clock (bro_json_value_get_string (v, ""), &d);
    g_autoptr (BroJsonValue) seconds = ok ? bro_json_value_new_integer ((gint64) d.seconds) : bro_json_value_new_null ();
    bro_test_same_json (failures, id, "seconds", bro_json_value_member (expect, "seconds"), seconds);
    g_autofree char *s = ok ? bro_duration_format_clock (d) : NULL;
    bro_test_same_string (failures, id, "formatted", bro_json_value_get_string (bro_json_value_member (expect, "formatted"), NULL), s);
    return TRUE;
  }
  if ((v = bro_json_value_member (given, "json"))) {
    g_autoptr (BroJsonValue) parsed = bro_json_value_parse (bro_json_value_get_string (v, ""), -1);
    g_autofree char *s = parsed ? bro_json_value_to_text (parsed) : NULL;
    bro_test_same_string (failures, id, "canonical", bro_json_value_get_string (bro_json_value_member (expect, "canonical"), NULL), s);
    return TRUE;
  }
  return FALSE;
}

static void
test_foundation_cases (void)
{
  bro_test_run_cases ("foundation/foundation.cases.json", foundation_case);
}

static void
test_json_values_compare_by_content (void)
{
  g_autoptr (BroJsonValue) a = bro_json_value_parse ("{\"a\": [1, {\"b\": null}]}", -1);
  g_autoptr (BroJsonValue) b = bro_json_value_parse ("{ \"a\" : [ 1 , { \"b\" : null } ] }", -1);
  g_autoptr (BroJsonValue) c = bro_json_value_parse ("{\"a\": 1, \"b\": 2}", -1);
  g_autoptr (BroJsonValue) d = bro_json_value_parse ("{\"b\": 2, \"a\": 1}", -1);
  g_autoptr (BroJsonValue) one = bro_json_value_parse ("1", -1);
  g_autoptr (BroJsonValue) one_point_zero = bro_json_value_parse ("1.0", -1);
  g_assert_true (bro_json_value_equal (a, b));
  g_assert_false (bro_json_value_equal (c, d));
  g_assert_false (bro_json_value_equal (one, one_point_zero));
  g_assert_null (bro_json_value_parse ("\"\xff\"", -1));
}

static void
count_call (gpointer data, gpointer token)
{
  (*(int *) data)++;
}

static void
test_cancellation_runs_handlers_once (void)
{
  g_autoptr (BroCancellationToken) token = bro_cancellation_token_new ();
  g_autoptr (BroCancellationToken) child = bro_cancellation_token_child (token);
  int calls = 0;
  bro_cancellation_token_on_cancel (child, count_call, &calls, NULL);
  g_assert_false (bro_cancellation_token_is_cancelled (child));
  bro_cancellation_token_cancel (token);
  bro_cancellation_token_cancel (token);
  g_assert_true (bro_cancellation_token_is_cancelled (child));
  g_assert_cmpint (calls, ==, 1);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/foundation/cases", test_foundation_cases);
  g_test_add_func ("/foundation/json-equality", test_json_values_compare_by_content);
  g_test_add_func ("/foundation/cancellation", test_cancellation_runs_handlers_once);
  return g_test_run ();
}
