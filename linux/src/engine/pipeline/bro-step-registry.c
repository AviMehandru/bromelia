/* bro-step-registry.c */
#include "bro-step-registry.h"

struct _BroStepRegistry {
  GObject parent_instance;
  GHashTable *steps; /* GINT_TO_POINTER (kind) → BroStep */
};

G_DEFINE_FINAL_TYPE (BroStepRegistry, bro_step_registry, G_TYPE_OBJECT)

BroStepRegistry *
bro_step_registry_new (GPtrArray *steps)
{
  BroStepRegistry *self = g_object_new (BRO_TYPE_STEP_REGISTRY, NULL);
  for (guint i = 0; steps && i < steps->len; i++)
    {
      BroStep *s = g_ptr_array_index (steps, i);
      g_hash_table_replace (self->steps, GINT_TO_POINTER (bro_step_kind (s)), g_object_ref (s));
    }
  return self;
}

BroStep *
bro_step_registry_step (BroStepRegistry *self, BroStepKind kind)
{
  return g_hash_table_lookup (self->steps, GINT_TO_POINTER (kind));
}

static void
bro_step_registry_finalize (GObject *object)
{
  g_hash_table_unref (BRO_STEP_REGISTRY (object)->steps);
  G_OBJECT_CLASS (bro_step_registry_parent_class)->finalize (object);
}

static void
bro_step_registry_class_init (BroStepRegistryClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_step_registry_finalize;
}

static void
bro_step_registry_init (BroStepRegistry *self)
{
  self->steps = g_hash_table_new_full (NULL, NULL, NULL, g_object_unref);
}
