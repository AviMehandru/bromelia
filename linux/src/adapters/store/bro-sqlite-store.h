/* bro-sqlite-store.h: BroSqliteStore: the Store port on SQLite (plan §10.4, §23; shared/fixtures/adapters/store.cases.json).
 * The shared migrations run verbatim (compiled in by tools/gen-migrations.py); the connection uses WAL,
 * synchronous=FULL and foreign keys. One connection, every call serialised (a read pool can come when measurements
 * ask for it). Times are ISO 8601 text (bro_instant_format), enums their wire names, JSON columns the canonical
 * encoding: BroError {code, params?, cause?}, BroMessage {code, params?, severity? (when not info)}, JobRequest
 * {kind, title, automatic, driveId?, mediaGeneration?, sessionId?, parentId?, unitId?, options?, details?}, JobPlan
 * {steps, mode?, profileId?, libraryId?, details?}, StepOutput {kind, data, summary?}.
 *
 * The object implements the Store port and every repository port: bro_store_jobs () and the others return it. */
#pragma once

#include "bro-clock.h"
#include "bro-store.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_SQLITE_STORE (bro_sqlite_store_get_type ())
G_DECLARE_FINAL_TYPE (BroSqliteStore, bro_sqlite_store, BRO, SQLITE_STORE, GObject)

/* Opens (creating) the database file; ":memory:" for a private in-memory one (tests). Doesn't migrate. NULL and
 * @error set (store.failed) when it can't be opened. Keeps a reference to @clock. */
BroSqliteStore *bro_sqlite_store_open (const char *path, BroClock *clock, BroBroError **error);

/* Applies the migrations whose number is above PRAGMA user_version, each in one transaction. */
gboolean bro_sqlite_store_migrate (BroSqliteStore *self, BroBroError **error);

/* BEGIN IMMEDIATE … COMMIT around @block, which must use the store it is given (the store's lock is held
 * meanwhile); FALSE from the block rolls back. */
gboolean bro_sqlite_store_transaction (BroSqliteStore *self, BroStoreBlockFunc block, gpointer data, BroBroError **error);

/* Closes the connection; later calls fail. Also done when the last reference goes. */
void bro_sqlite_store_close (BroSqliteStore *self);

G_END_DECLS
