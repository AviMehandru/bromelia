/* bro-test-fixtures.h: reads the shared golden fixtures ($BROMELIA_FIXTURES, i.e. shared/fixtures) and runs
 * their cases. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

char *bro_test_fixture_path (const char *relative);
char *bro_test_fixture_text (const char *relative, gsize *length);
BroJsonValue *bro_test_fixture_json (const char *relative);

/* One case: returns TRUE when it knows the case. On a mismatch it adds a line to @failures (bro_test_fail). */
typedef gboolean (*BroTestCaseFunc) (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures);

/* Runs every case of a *.cases.json file and fails the test (listing every failed case) unless all pass. A
 * case no function knows fails too: no case is skipped silently. */
void bro_test_run_cases (const char *relative, BroTestCaseFunc run);

/* The same for the cases whose id passes @only: for a file whose other cases belong to a module that isn't
 * built yet. */
void bro_test_run_cases_only (const char *relative, gboolean (*only) (const char *id), BroTestCaseFunc run);

void bro_test_fail (GPtrArray *failures, const char *id, const char *format, ...) G_GNUC_PRINTF (3, 4);

/* Compares and records a failure; return TRUE when equal. NULL is a value (JSON null). */
gboolean bro_test_same_string (GPtrArray *failures, const char *id, const char *what, const char *expected, const char *actual);
gboolean bro_test_same_json (GPtrArray *failures, const char *id, const char *what, const BroJsonValue *expected, const BroJsonValue *actual);

G_END_DECLS
