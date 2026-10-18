/* bro-random-id-generator.c */
#include "bro-random-id-generator.h"

struct _BroRandomIdGenerator {
  GObject parent_instance;
};

static void id_generator_iface_init (BroIdGeneratorInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (BroRandomIdGenerator, bro_random_id_generator, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (BRO_TYPE_ID_GENERATOR, id_generator_iface_init))

static void
bro_random_id_generator_class_init (BroRandomIdGeneratorClass *klass)
{
}

static void
bro_random_id_generator_init (BroRandomIdGenerator *self)
{
}

BroRandomIdGenerator *
bro_random_id_generator_new (void)
{
  return g_object_new (BRO_TYPE_RANDOM_ID_GENERATOR, NULL);
}

BroId
bro_random_id_generator_next (BroRandomIdGenerator *self)
{
  BroId id = { { 0 } };
  g_autofree char *text = g_uuid_string_random ();
  g_strlcpy (id.value, text, sizeof id.value);
  return id;
}

static BroId
next_id (BroIdGenerator *self)
{
  return bro_random_id_generator_next (BRO_RANDOM_ID_GENERATOR (self));
}

static void
id_generator_iface_init (BroIdGeneratorInterface *iface)
{
  iface->next = next_id;
}
