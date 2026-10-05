/* bro-run-product.h: what a makemkvcon run should leave in its destination: nothing (a listing), titles (MKV files of
 * a rip) or a backup (a disc structure or an ISO image). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  BRO_RUN_PRODUCT_NOTHING,
  BRO_RUN_PRODUCT_TITLES,
  BRO_RUN_PRODUCT_BACKUP,
} BroRunProduct;

/* The wire form ("titles"). */
const char *bro_run_product_to_wire (BroRunProduct value);
gboolean bro_run_product_from_wire (const char *text, BroRunProduct *out);

G_END_DECLS
