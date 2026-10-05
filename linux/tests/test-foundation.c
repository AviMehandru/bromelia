/* test-foundation.c: Foundation against shared/fixtures/foundation. */
#include "bro-bro-error.h"
#include "bro-cancellation-source.h"
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
  g_autoptr (BroCancellationSource) parent = bro_cancellation_source_new ();
  g_autoptr (BroCancellationSource) child = bro_cancellation_source_linked (bro_cancellation_source_token (parent));
  int calls = 0;
  bro_cancellation_token_on_cancel (bro_cancellation_source_token (child), count_call, &calls, NULL);
  g_assert_false (bro_cancellation_token_is_cancelled (bro_cancellation_source_token (child)));
  bro_cancellation_source_cancel (parent);
  bro_cancellation_source_cancel (parent);
  g_assert_true (bro_cancellation_token_is_cancelled (bro_cancellation_source_token (child)));
  g_assert_cmpint (calls, ==, 1);
}

/* A closed (or freed) linked source no longer follows its parent, and the parent no longer holds its token: a parent
 * that lives long doesn't keep one per child. */
static void
test_closed_linked_source_lets_go (void)
{
  g_autoptr (BroCancellationSource) parent = bro_cancellation_source_new ();
  g_autoptr (BroCancellationSource) child = bro_cancellation_source_linked (bro_cancellation_source_token (parent));
  g_autoptr (BroCancellationSource) late = NULL;
  BroCancellationToken *token = bro_cancellation_token_ref (bro_cancellation_source_token (child));
  bro_cancellation_source_free (bro_cancellation_source_linked (bro_cancellation_source_token (parent)));
  bro_cancellation_source_close (child);
  bro_cancellation_source_cancel (parent);
  g_assert_false (bro_cancellation_token_is_cancelled (token));
  bro_cancellation_token_unref (token);
  late = bro_cancellation_source_linked (bro_cancellation_source_token (parent));
  g_assert_true (bro_cancellation_token_is_cancelled (bro_cancellation_source_token (late)));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/foundation/cases", test_foundation_cases);
  g_test_add_func ("/foundation/json-equality", test_json_values_compare_by_content);
  g_test_add_func ("/foundation/cancellation", test_cancellation_runs_handlers_once);
  g_test_add_func ("/foundation/closed-linked-source", test_closed_linked_source_lets_go);
  return g_test_run ();
}
