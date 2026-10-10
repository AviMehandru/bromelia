/* bro-coalescer.h: BroCoalescer: at most one value per interval per key (plan §8 rule 5): a job's progress before
 * it becomes an event or is stored. A value goes out at once when nothing for its key went out in the last
 * interval; otherwise it waits (replacing the one waiting) until one interval after the last one went out. Used on
 * the executor. Today's macOS EventSink did this for robot progress; shared/fixtures/engine/coalescer.cases.json. */
#pragma once

#include "bro-engine-clock.h"
#include "bro-json-value.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_COALESCER (bro_coalescer_get_type ())
G_DECLARE_FINAL_TYPE (BroCoalescer, bro_coalescer, BRO, COALESCER, GObject)

/* @value is borrowed. */
typedef void (*BroCoalescerEmit) (const char *key, BroJsonValue *value, gpointer data);

BroCoalescer *bro_coalescer_new (BroEngineClock *clock, BroDuration interval, BroCoalescerEmit emit, gpointer data,
                                 GDestroyNotify destroy);

/* Takes a reference to @value. */
void bro_coalescer_offer (BroCoalescer *self, const char *key, BroJsonValue *value);

/* Sends a waiting value at once. */
void bro_coalescer_flush (BroCoalescer *self, const char *key);

/* Throws a waiting value away and forgets when the key last went out. */
void bro_coalescer_drop (BroCoalescer *self, const char *key);

G_END_DECLS
