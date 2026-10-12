/* bro-problem-service.c */
#include "bro-problem-service.h"

#include <string.h>

struct _BroProblemService {
  GObject parent_instance;
  BroEventBus *bus;
  BroClock *clock;
  GPtrArray *problems; /* BroProblemDto *, owned */
};

G_DEFINE_FINAL_TYPE (BroProblemService, bro_problem_service, G_TYPE_OBJECT)

static char *
key (BroMessageCode code, const BroProblemScope *scope)
{
  if (!scope)
    return g_strdup (bro_message_code_wire (code));
  return g_strconcat (bro_message_code_wire (code), ":", bro_problem_scope_kind_to_wire (scope->kind), ":", scope->id, NULL);
}

static int
index_of (BroProblemService *self, const char *id)
{
  for (guint i = 0; i < self->problems->len; i++)
    if (strcmp (((BroProblemDto *) g_ptr_array_index (self->problems, i))->id, id) == 0)
      return (int) i;
  return -1;
}

BroProblemService *
bro_problem_service_new (BroEventBus *bus, BroClock *clock)
{
  BroProblemService *self = g_object_new (BRO_TYPE_PROBLEM_SERVICE, NULL);
  self->bus = g_object_ref (bus);
  self->clock = g_object_ref (clock);
  return self;
}

GPtrArray *
bro_problem_service_problems (BroProblemService *self)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_problem_dto_free);
  for (guint i = 0; i < self->problems->len; i++)
    g_ptr_array_add (copy, bro_problem_dto_copy (g_ptr_array_index (self->problems, i)));
  return copy;
}

BroProblemDto *
bro_problem_service_raise (BroProblemService *self, const BroBroMessage *message, const BroProblemScope *scope,
                           const BroProblemAction *actions, gsize n_actions)
{
  g_autofree char *id = key (message->code, scope);
  int i = index_of (self, id);
  BroInstant raised_at = i >= 0 ? ((BroProblemDto *) g_ptr_array_index (self->problems, i))->raised_at : bro_clock_now (self->clock);
  BroProblemDto *problem = bro_problem_dto_new (id, message, scope, raised_at, actions, n_actions);
  g_autoptr (BroJsonValue) json = bro_problem_dto_json (problem);
  g_autoptr (BroJsonValue) data = NULL;
  if (i >= 0)
    {
      BroProblemDto *old = g_ptr_array_index (self->problems, i);
      g_autoptr (BroJsonValue) was = bro_problem_dto_json (old);
      if (bro_json_value_equal (was, json))
        {
          bro_problem_dto_free (problem);
          return bro_problem_dto_copy (old);
        }
      bro_problem_dto_free (old);
      self->problems->pdata[i] = problem;
    }
  else
    g_ptr_array_add (self->problems, problem);
  data = bro_json_value_new_object ();
  bro_json_value_set (data, "problem", g_steal_pointer (&json));
  bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_PROBLEM_RAISED, data));
  return bro_problem_dto_copy (problem);
}

void
bro_problem_service_clear (BroProblemService *self, BroMessageCode code, const BroProblemScope *scope)
{
  g_autofree char *id = key (code, scope);
  g_autoptr (BroJsonValue) data = NULL;
  int i = index_of (self, id);
  if (i < 0)
    return;
  g_ptr_array_remove_index (self->problems, i);
  data = bro_json_value_new_object ();
  bro_json_value_set (data, "problemId", bro_json_value_new_string (id));
  bro_json_value_set (data, "code", bro_json_value_new_string (bro_message_code_wire (code)));
  bro_engine_event_unref (bro_event_bus_publish (self->bus, BRO_ENGINE_EVENT_TYPE_PROBLEM_CLEARED, data));
}

static void
bro_problem_service_finalize (GObject *object)
{
  BroProblemService *self = BRO_PROBLEM_SERVICE (object);
  g_ptr_array_unref (self->problems);
  g_object_unref (self->bus);
  g_object_unref (self->clock);
  G_OBJECT_CLASS (bro_problem_service_parent_class)->finalize (object);
}

static void
bro_problem_service_class_init (BroProblemServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_problem_service_finalize;
}

static void
bro_problem_service_init (BroProblemService *self)
{
  self->problems = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_problem_dto_free);
}
