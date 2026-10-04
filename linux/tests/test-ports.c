/* test-ports.c: the Ports layer: its enums match the database, and a port can be implemented. */
#include "bro-test-fixtures.h"

#include "bro-check-result.h"
#include "bro-job-state.h"
#include "bro-keystore.h"
#include "bro-media-kind.h"
#include "bro-outcome.h"
#include "bro-queue.h"
#include "bro-replica-state.h"
#include "bro-step-state.h"
#include "bro-unit-state.h"
#include "bro-unit-status.h"

#include <string.h>

/* ---- a keystore in memory, through the port ---------------------------------------------------------- */

#define TEST_TYPE_KEYSTORE (test_keystore_get_type ())
G_DECLARE_FINAL_TYPE (TestKeystore, test_keystore, TEST, KEYSTORE, GObject)

struct _TestKeystore {
  GObject parent_instance;
  GHashTable *secrets;
};

static void test_keystore_iface_init (BroKeystoreInterface *iface);

G_DEFINE_TYPE_WITH_CODE (TestKeystore, test_keystore, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_KEYSTORE, test_keystore_iface_init))

static char *
keystore_get (BroKeystore *self, const char *name, BroBroError **error)
{
  return g_strdup (g_hash_table_lookup (TEST_KEYSTORE (self)->secrets, name));
}

static gboolean
keystore_set (BroKeystore *self, const char *name, const char *value, BroBroError **error)
{
  g_hash_table_replace (TEST_KEYSTORE (self)->secrets, g_strdup (name), g_strdup (value));
  return TRUE;
}

static gboolean
keystore_remove (BroKeystore *self, const char *name, BroBroError **error)
{
  g_hash_table_remove (TEST_KEYSTORE (self)->secrets, name);
  return TRUE;
}

static void
test_keystore_iface_init (BroKeystoreInterface *iface)
{
  iface->get = keystore_get;
  iface->set = keystore_set;
  iface->remove = keystore_remove;
}

static void
test_keystore_finalize (GObject *object)
{
  g_hash_table_unref (TEST_KEYSTORE (object)->secrets);
  G_OBJECT_CLASS (test_keystore_parent_class)->finalize (object);
}

static void
test_keystore_class_init (TestKeystoreClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = test_keystore_finalize;
}

static void
test_keystore_init (TestKeystore *self)
{
  self->secrets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

static void
test_a_port_can_be_implemented (void)
{
  g_autoptr (BroKeystore) keystore = BRO_KEYSTORE (g_object_new (TEST_TYPE_KEYSTORE, NULL));
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *none = bro_keystore_get (keystore, "metadata.apiKey", &error);
  g_assert_null (none);
  g_assert_true (bro_keystore_set (keystore, "metadata.apiKey", "k", &error));
  g_autofree char *k = bro_keystore_get (keystore, "metadata.apiKey", &error);
  g_assert_cmpstr (k, ==, "k");
  g_assert_true (bro_keystore_remove (keystore, "metadata.apiKey", &error));
  g_autofree char *gone = bro_keystore_get (keystore, "metadata.apiKey", &error);
  g_assert_null (gone);
  g_assert_null (error);
}

/* ---- enums against shared/schema/db -------------------------------------------------------------------- */

/* The values a column's CHECK (column IN (…)) allows in shared/schema/db/0001_init.sql, joined with ",". */
static char *
allowed (const char *table, const char *column)
{
  g_autofree char *sql = bro_test_fixture_text ("../schema/db/0001_init.sql", NULL);
  g_autofree char *table_re = g_strdup_printf ("CREATE TABLE %s \\((.*?)\\n\\) STRICT;", table);
  g_autoptr (GRegex) tre = g_regex_new (table_re, G_REGEX_DOTALL, 0, NULL);
  g_autoptr (GMatchInfo) tm = NULL;
  g_assert_true (g_regex_match (tre, sql, 0, &tm));
  g_autofree char *block = g_match_info_fetch (tm, 1);
  g_autofree char *col_re = g_strdup_printf ("CHECK \\(%s(?: IS NULL OR %s)? IN \\(([^)]*)\\)\\)", column, column);
  g_autoptr (GRegex) cre = g_regex_new (col_re, 0, 0, NULL);
  g_autoptr (GMatchInfo) cm = NULL;
  g_assert_true (g_regex_match (cre, block, 0, &cm));
  g_autofree char *list = g_match_info_fetch (cm, 1);
  g_autoptr (GRegex) vre = g_regex_new ("'([^']*)'", 0, 0, NULL);
  g_autoptr (GMatchInfo) vm = NULL;
  GString *out = g_string_new (NULL);
  for (g_regex_match (vre, list, 0, &vm); g_match_info_matches (vm); g_match_info_next (vm, NULL)) {
    g_autofree char *v = g_match_info_fetch (vm, 1);
    g_string_append_printf (out, "%s%s", out->len ? "," : "", v);
  }
  return g_string_free (out, FALSE);
}

typedef const char *(*WireFunc) (int value);

static char *
wire (WireFunc to_wire, int count)
{
  GString *out = g_string_new (NULL);
  for (int i = 0; i < count; i++)
    g_string_append_printf (out, "%s%s", i ? "," : "", to_wire (i));
  return g_string_free (out, FALSE);
}

#define SAME(table, column, func, count)                                                                                \
  do {                                                                                                                  \
    g_autofree char *want = allowed (table, column), *got = wire ((WireFunc) (func), (count));                          \
    g_assert_cmpstr (got, ==, want);                                                                                    \
  } while (0)

static void
test_enums_match_the_database (void)
{
  SAME ("archive_units", "state", bro_unit_state_to_wire, BRO_UNIT_STATE_MISSING + 1);
  SAME ("archive_units", "status", bro_unit_status_to_wire, BRO_UNIT_STATUS_INCOMPLETE + 1);
  SAME ("replicas", "state", bro_replica_state_to_wire, BRO_REPLICA_STATE_FAILED + 1);
  SAME ("jobs", "state", bro_job_state_to_wire, BRO_JOB_STATE_FINISHED + 1);
  SAME ("jobs", "outcome", bro_outcome_to_wire, BRO_OUTCOME_INTERRUPTED + 1);
  SAME ("jobs", "queue", bro_queue_to_wire, BRO_QUEUE_MAINTENANCE + 1);
  SAME ("job_steps", "state", bro_step_state_to_wire, BRO_STEP_STATE_INTERRUPTED + 1);
  SAME ("checks", "result", bro_check_result_to_wire, BRO_CHECK_RESULT_STOPPED + 1);
  SAME ("works", "kind", bro_media_kind_to_wire, BRO_MEDIA_KIND_TV + 1);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ports/enums-match-the-database", test_enums_match_the_database);
  g_test_add_func ("/ports/implemented", test_a_port_can_be_implemented);
  return g_test_run ();
}
