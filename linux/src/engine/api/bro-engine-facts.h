/* bro-engine-facts.h: BroEngineFacts: what the host knows and the engine can't ask the system for (the composition
 * root fills it): the app's version, the process id, the platform (macos, windows or linux), the data folder, the
 * processor count (limits.cpu 0 = cores / 2). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *version;
  gint64 pid;
  char *platform;
  char *data_dir;
  int cores;
} BroEngineFacts;

/* Copies the strings. */
BroEngineFacts *bro_engine_facts_new (const char *version, gint64 pid, const char *platform, const char *data_dir, int cores);
BroEngineFacts *bro_engine_facts_copy (const BroEngineFacts *facts);
void bro_engine_facts_free (BroEngineFacts *facts);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroEngineFacts, bro_engine_facts_free)

G_END_DECLS
