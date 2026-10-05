/* bro-mkv-tool-nix.c */
#include "bro-mkv-tool-nix.h"

#include "bro-message-code.h"
#include "bro-simple-chapters.h"
#include "bro-split.h"
#include "bro-tool-run.h"
#include <string.h>

struct _BroMkvToolNix {
  GObject parent_instance;
  BroProcessLauncher *launcher;
  BroFileSystem *fs;
  BroToolLocator *locator;
  char *work_directory;
};

G_DEFINE_FINAL_TYPE (BroMkvToolNix, bro_mkv_tool_nix, G_TYPE_OBJECT)

/* Keeps the stdout lines (when @lines isn't NULL). */
static gboolean
keep_stdout (const BroOutputLine *line, gpointer lines, BroStopReason *reason)
{
  if (lines && line->stream == BRO_OUTPUT_SOURCE_STDOUT)
    g_ptr_array_add (lines, g_strdup (line->text));
  return FALSE;
}

/* Runs @tool; its stdout lines go to @lines (when not NULL). FALSE and @error set when it can't run or was
 * cancelled; otherwise *@exit tells how it ended. */
static gboolean
run (BroMkvToolNix *self, BroToolKind tool, const char *const *arguments, double stall, BroCancellationToken *cancel, GPtrArray *lines,
     BroProcessExit *exit, BroBroError **error)
{
  g_autoptr (BroToolInfo) info = bro_tool_locator_locate (self->locator, tool);
  g_autoptr (BroProcessSpec) spec = NULL;
  if (!info->path)
    {
      if (error)
        {
          if (info->why)
            *error = bro_bro_message_to_error (info->why, NULL);
          else
            {
              BroJsonValue *params = bro_json_value_new_object ();
              bro_json_value_set (params, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (tool)));
              bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_TOOL_MISSING), params);
            }
        }
      return FALSE;
    }
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return FALSE;
    }
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (info->path);
  g_strfreev (spec->arguments);
  spec->arguments = g_strdupv ((char **) arguments);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  spec->has_stall_timeout = TRUE;
  spec->stall_timeout.seconds = stall;
  if (!bro_tool_run (self->launcher, spec, cancel, NULL, keep_stdout, lines, exit, NULL, error))
    return FALSE;
  if (exit->cancelled)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return FALSE;
    }
  return TRUE;
}

BroMkvProbe *
bro_mkv_tool_nix_probe (BroMkvToolNix *self, const char *file, BroCancellationToken *cancel, BroBroError **error)
{
  const char *args[] = { "-J", file, NULL };
  g_autoptr (GPtrArray) lines = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *json = NULL;
  BroProcessExit exit;
  if (!run (self, BRO_TOOL_KIND_MKVMERGE, args, 300, cancel, lines, &exit, error) || exit.status > 1)
    return NULL;
  g_ptr_array_add (lines, NULL);
  json = g_strjoinv ("\n", (char **) lines->pdata);
  return bro_mkv_probe_parse (json);
}

gboolean
bro_mkv_tool_nix_remux (BroMkvToolNix *self, const char *const *arguments, BroCancellationToken *cancel, BroBroError **error)
{
  BroProcessExit exit;
  return run (self, BRO_TOOL_KIND_MKVMERGE, arguments, 600, cancel, NULL, &exit, error) && exit.status <= 1;
}

static int
compare_names (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

GStrv
bro_mkv_tool_nix_split (BroMkvToolNix *self, GArray *chapters, const char *input, BroCancellationToken *cancel, BroBroError **error)
{
  g_autofree char *folder = g_path_get_dirname (input);
  g_autofree char *random = g_strdup_printf ("%08x", g_random_int ());
  g_autofree char *prefix = g_strconcat (".bromelia-split-", random, NULL);
  g_autofree char *pattern = g_strdup_printf ("%s/%s-%%03d.mkv", folder, prefix);
  g_auto (GStrv) args = bro_split_arguments (chapters, input, pattern);
  g_autoptr (GPtrArray) entries = NULL;
  GPtrArray *parts = g_ptr_array_new_with_free_func (g_free);
  BroProcessExit exit = { 0 };
  gboolean ran = run (self, BRO_TOOL_KIND_MKVMERGE, (const char *const *) args, 600, cancel, NULL, &exit, error);
  entries = bro_file_system_list (self->fs, folder, NULL);
  for (guint i = 0; entries && i < entries->len; i++)
    {
      const char *name = ((BroDirectoryEntry *) entries->pdata[i])->name;
      if (g_str_has_prefix (name, prefix))
        g_ptr_array_add (parts, g_strdup (name));
    }
  g_ptr_array_sort (parts, compare_names);
  for (guint i = 0; i < parts->len; i++)
    {
      char *full = g_build_filename (folder, parts->pdata[i], NULL);
      g_free (parts->pdata[i]);
      parts->pdata[i] = full;
    }
  if (ran && exit.status <= 1 && parts->len == chapters->len + 1)
    {
      g_ptr_array_add (parts, NULL);
      return (GStrv) g_ptr_array_free (parts, FALSE);
    }
  for (guint i = 0; i < parts->len; i++)
    bro_file_system_remove (self->fs, parts->pdata[i], NULL);
  g_ptr_array_unref (parts);
  return NULL;
}

GArray *
bro_mkv_tool_nix_chapter_times (BroMkvToolNix *self, const char *file, BroCancellationToken *cancel, BroBroError **error)
{
  g_autofree char *random = g_strdup_printf ("%08x", g_random_int ());
  g_autofree char *name = g_strconcat (".bromelia-chapters-", random, ".txt", NULL);
  g_autofree char *temp = g_build_filename (self->work_directory, name, NULL);
  const char *args[] = { file, "chapters", "--simple", temp, NULL };
  g_autoptr (GBytes) bytes = NULL;
  g_autofree char *text = NULL;
  BroProcessExit exit;
  GArray *times = NULL;
  if (run (self, BRO_TOOL_KIND_MKVEXTRACT, args, 120, cancel, NULL, &exit, error) && exit.status <= 1
      && (bytes = bro_file_system_read (self->fs, temp, NULL)) != NULL)
    {
      gsize n = 0;
      const char *data = g_bytes_get_data (bytes, &n);
      text = g_strndup (data ? data : "", n);
      times = bro_simple_chapters_parse (text);
    }
  if (bro_file_system_exists (self->fs, temp))
    bro_file_system_remove (self->fs, temp, NULL);
  return times;
}

static void
bro_mkv_tool_nix_finalize (GObject *object)
{
  BroMkvToolNix *self = BRO_MKV_TOOL_NIX (object);
  g_clear_object (&self->launcher);
  g_clear_object (&self->fs);
  g_clear_object (&self->locator);
  g_free (self->work_directory);
  G_OBJECT_CLASS (bro_mkv_tool_nix_parent_class)->finalize (object);
}

static void
bro_mkv_tool_nix_class_init (BroMkvToolNixClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_mkv_tool_nix_finalize;
}

static void
bro_mkv_tool_nix_init (BroMkvToolNix *self)
{
}

BroMkvToolNix *
bro_mkv_tool_nix_new (BroProcessLauncher *launcher, BroFileSystem *fs, BroToolLocator *locator, const char *work_directory)
{
  BroMkvToolNix *self = g_object_new (BRO_TYPE_MKV_TOOL_NIX, NULL);
  self->launcher = g_object_ref (launcher);
  self->fs = g_object_ref (fs);
  self->locator = g_object_ref (locator);
  self->work_directory = g_strdup (work_directory);
  return self;
}
