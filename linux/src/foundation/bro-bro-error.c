/* bro-bro-error.c */
#include "bro-bro-error.h"

BroBroError *
bro_bro_error_new (const char *code, BroJsonValue *params, BroBroError *cause)
{
  BroBroError *e = g_new0 (BroBroError, 1);
  e->code = g_strdup (code);
  e->params = params ? params : bro_json_value_new_object ();
  e->cause = cause;
  return e;
}

BroBroError *
bro_bro_error_copy (const BroBroError *error)
{
  if (!error)
    return NULL;
  return bro_bro_error_new (error->code, bro_json_value_ref (error->params), bro_bro_error_copy (error->cause));
}

void
bro_bro_error_free (BroBroError *error)
{
  if (!error)
    return;
  g_free (error->code);
  bro_json_value_unref (error->params);
  bro_bro_error_free (error->cause);
  g_free (error);
}

void
bro_bro_error_set (BroBroError **error, const char *code, BroJsonValue *params)
{
  if (!error) {
    bro_json_value_unref (params);
    return;
  }
  g_warn_if_fail (*error == NULL);
  *error = bro_bro_error_new (code, params, NULL);
}
