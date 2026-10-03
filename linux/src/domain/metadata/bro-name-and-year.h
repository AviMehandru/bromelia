/* bro-name-and-year.h: a name and the year typed after it: "Inception (2010)". */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char *name;
  int year; /* -1: none */
} BroNameAndYear;

void bro_name_and_year_free (BroNameAndYear *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroNameAndYear, bro_name_and_year_free)

G_END_DECLS
