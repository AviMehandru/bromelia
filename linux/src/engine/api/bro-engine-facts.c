/* bro-engine-facts.c */
#include "bro-engine-facts.h"

BroEngineFacts *
bro_engine_facts_new (const char *version, gint64 pid, const char *platform, const char *data_dir, int cores)
{
  BroEngineFacts *f = g_new0 (BroEngineFacts, 1);
  f->version = g_strdup (version);
  f->pid = pid;
  f->platform = g_strdup (platform);
  f->data_dir = g_strdup (data_dir);
  f->cores = cores;
  return f;
}

BroEngineFacts *
bro_engine_facts_copy (const BroEngineFacts *facts)
{
  return bro_engine_facts_new (facts->version, facts->pid, facts->platform, facts->data_dir, facts->cores);
}

void
bro_engine_facts_free (BroEngineFacts *facts)
{
  if (!facts)
    return;
  g_free (facts->version);
  g_free (facts->platform);
  g_free (facts->data_dir);
  g_free (facts);
}
