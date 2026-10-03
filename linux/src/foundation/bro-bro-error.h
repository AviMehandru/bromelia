/* bro-bro-error.h: an expected failure (plan §16): a message code in its wire form ("library.offline"), its
 * parameters and the failure that caused it. Functions that can fail return FALSE / NULL and set a
 * BroBroError ** out-parameter. Domain's BroMessageCode gives the codes their names. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

typedef struct _BroBroError BroBroError;

struct _BroBroError {
  char *code;
  BroJsonValue *params; /* an object, never NULL */
  BroBroError *cause;   /* nullable */
};

/* Takes ownership of @params (NULL → an empty object) and @cause. */
BroBroError *bro_bro_error_new (const char *code, BroJsonValue *params, BroBroError *cause);
BroBroError *bro_bro_error_copy (const BroBroError *error);
void bro_bro_error_free (BroBroError *error);

/* Sets *@error (when @error isn't NULL) like g_set_error; takes ownership of @params. */
void bro_bro_error_set (BroBroError **error, const char *code, BroJsonValue *params);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroBroError, bro_bro_error_free)

G_END_DECLS
