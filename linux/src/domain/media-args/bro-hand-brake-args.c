/* bro-hand-brake-args.c */
#include "bro-hand-brake-args.h"

#include "bro-argument-splitter.h"
#include "bro-media-args-private.h"
#include "bro-template-engine.h"

#include <stdlib.h>
#include <string.h>

GStrv
bro_hand_brake_args_build (const BroStepDefinition *step, const char *input, const char *output, const char *home)
{
  const BroHandBrakeSettings *h = &step->handbrake;
  GPtrArray *args = g_ptr_array_new ();
  g_autofree char *trimmed = g_strstrip (g_strdup (h->preset_file ? h->preset_file : ""));
  char *file = _bro_media_args_expand_home (trimmed, home);
  if (*file) {
    g_ptr_array_add (args, g_strdup ("--preset-import-file"));
    g_ptr_array_add (args, file);
  } else {
    g_free (file);
  }
  char *preset = g_strstrip (g_strdup (h->preset ? h->preset : ""));
  if (*preset) {
    g_ptr_array_add (args, g_strdup ("--preset"));
    g_ptr_array_add (args, preset);
  } else {
    g_free (preset);
  }
  g_ptr_array_add (args, g_strdup ("-i"));
  g_ptr_array_add (args, g_strdup (input));
  g_ptr_array_add (args, g_strdup ("-o"));
  g_ptr_array_add (args, g_strdup (output));
  g_auto (GStrv) extra = bro_argument_splitter_split (h->extra_arguments ? h->extra_arguments : "");
  for (int i = 0; extra[i]; i++)
    g_ptr_array_add (args, g_strdup (extra[i]));
  g_ptr_array_add (args, NULL);
  return (GStrv) g_ptr_array_free (args, FALSE);
}

char *
bro_hand_brake_args_output (const BroStepDefinition *step, GHashTable *values, const char *home)
{
  g_autofree char *t = g_strstrip (g_strdup (step->handbrake.output_path ? step->handbrake.output_path : ""));
  g_autofree char *rendered = bro_template_engine_render (*t ? t : "{outputDir}/Encoded/{stem}.mkv", values);
  return _bro_media_args_expand_home (rendered, home);
}

gboolean
bro_hand_brake_args_is_source (const char *path)
{
  gsize n = strlen (path);
  return n >= 4 && g_ascii_strcasecmp (path + n - 4, ".mkv") == 0;
}

gboolean
bro_hand_brake_args_keep_line (BroProgressFilter *filter, const char *line)
{
  g_autoptr (GRegex) re = g_regex_new ("Encoding: task (\\d+) of \\d+, (\\d+)\\.\\d+ %", 0, 0, NULL);
  g_autoptr (GMatchInfo) m = NULL;
  if (!g_regex_match (re, line, 0, &m))
    return TRUE;
  g_autofree char *t = g_match_info_fetch (m, 1), *p = g_match_info_fetch (m, 2);
  int task = atoi (t), step = atoi (p) / 10;
  if (task != filter->task) {
    filter->task = task;
    filter->next = 0;
  }
  if (step < filter->next)
    return FALSE;
  filter->next = step + 1;
  return TRUE;
}
