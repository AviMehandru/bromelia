/* bro-logic.h — title selection rules, {token} templates and argument splitting. */
#pragma once

#include <glib.h>
#include "bro-config.h"
#include "bro-robot.h"

G_BEGIN_DECLS

typedef struct {
  int title_index;
  gboolean selected;
  char *reason;
} BroTitleDecision;

typedef struct {
  GArray *decisions; /* BroTitleDecision, in title order */
  gboolean requires_manual;
  char *error;
} BroSelectionResult;

BroSelectionResult *bro_select_titles (BroDiscInfo *info, const BroTitleSelection *rule);
void                bro_selection_result_free (BroSelectionResult *r);
GArray             *bro_selection_result_selected (BroSelectionResult *r); /* int array */
gboolean            bro_selection_result_is_selected (BroSelectionResult *r, int title);
const char         *bro_selection_result_reason (BroSelectionResult *r, int title);

/* Index patterns: "0,2-4,7-", "last", "all", "*". Returns FALSE and sets error for invalid input. */
gboolean bro_index_pattern_validate (const char *pattern, char **error);
gboolean bro_index_pattern_matches (const char *pattern, int value, int max_value);

typedef struct { const char *token; const char *help; } BroTokenHelp;
extern const BroTokenHelp bro_folder_tokens[];
extern const BroTokenHelp bro_file_tokens[];
extern const BroTokenHelp bro_script_tokens[];

char       *bro_template_render (const char *tmpl, GHashTable *values, gboolean sanitize);
char       *bro_template_render_path (const char *tmpl, GHashTable *values);
char       *bro_sanitize_component (const char *s);
GHashTable *bro_template_values_new (void); /* includes date/time values */
void        bro_template_values_set (GHashTable *values, const char *key, const char *value);

GPtrArray *bro_split_arguments (const char *s);
char      *bro_quote_argument (const char *a);
char      *bro_command_line (const char *const *argv);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroSelectionResult, bro_selection_result_free)

G_END_DECLS
