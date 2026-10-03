/* bro-one-pass-plan.h: rip the chosen titles in one makemkvcon run, with this minimum title length (seconds). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  int min_length;
} BroOnePassPlan;

G_END_DECLS
