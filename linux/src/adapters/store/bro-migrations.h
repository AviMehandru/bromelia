/* bro-migrations.h: the migrations of shared/schema/db, compiled in by tools/gen-migrations.py (internal to the store). */
#pragma once

#include <stddef.h>

typedef struct {
  int version;
  const char *name;
  const char *sql;
} BroMigration;

/* In order; ends with an entry whose name is NULL. */
const BroMigration *_bro_migrations (void);
