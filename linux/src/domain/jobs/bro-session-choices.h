/* bro-session-choices.h: what the user chose on an opened disc (dtos.json's SessionChoices): every title with whether
 * it is ticked, the name typed for the disc ("" = from the disc), movie or TV, year, online id, first episode, library
 * and profile (NULL = the default). */
#pragma once

#include "bro-json-value.h"
#include "bro-media-kind.h"
#include "bro-title-choice.h"

G_BEGIN_DECLS

typedef struct {
  GPtrArray *titles; /* BroTitleChoice * */
  char *name;
  gboolean has_kind;
  BroMediaKind kind;
  gboolean has_year;
  int year;
  char *online_id;
  gboolean has_first_episode;
  int first_episode;
  char *library; /* nullable */
  char *profile; /* nullable */
} BroSessionChoices;

BroSessionChoices *bro_session_choices_new (void);
BroSessionChoices *bro_session_choices_copy (const BroSessionChoices *choices);
void bro_session_choices_free (BroSessionChoices *choices);

BroJsonValue *bro_session_choices_encode (const BroSessionChoices *choices);

/* The choices bro_session_choices_encode wrote; what the JSON leaves out takes its default. */
BroSessionChoices *bro_session_choices_decode (const BroJsonValue *json);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSessionChoices, bro_session_choices_free)

G_END_DECLS
