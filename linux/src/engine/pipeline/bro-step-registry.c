/* bro-step-registry.c */
#include "bro-step-registry.h"
#include "bro-acquire-step.h"
#include "bro-await-media-step.h"
#include "bro-commit-step.h"
#include "bro-decide-step.h"
#include "bro-identify-step.h"
#include "bro-keep-apart-step.h"
#include "bro-name-step.h"
#include "bro-notify-step.h"
#include "bro-plan-step.h"
#include "bro-probe-step.h"
#include "bro-reconcile-step.h"
#include "bro-release-step.h"
#include "bro-seal-step.h"
#include "bro-verify-rips-step.h"

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

BroStepRegistry *
bro_step_registry_standard (void)
{
  g_autoptr (GPtrArray) steps = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (steps, bro_await_media_step_new ());
  g_ptr_array_add (steps, bro_probe_step_new ());
  g_ptr_array_add (steps, bro_reconcile_step_new ());
  g_ptr_array_add (steps, bro_identify_step_new ());
  g_ptr_array_add (steps, bro_plan_step_new ());
  g_ptr_array_add (steps, bro_decide_step_new ());
  g_ptr_array_add (steps, bro_acquire_step_new ());
  g_ptr_array_add (steps, bro_release_step_new ());
  g_ptr_array_add (steps, bro_verify_rips_step_new ());
  g_ptr_array_add (steps, bro_name_step_new ());
  g_ptr_array_add (steps, bro_seal_step_new ());
  g_ptr_array_add (steps, bro_commit_step_new ());
  g_ptr_array_add (steps, bro_notify_step_new ());
  g_ptr_array_add (steps, bro_keep_apart_step_new ());
  return bro_step_registry_new (steps);
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
