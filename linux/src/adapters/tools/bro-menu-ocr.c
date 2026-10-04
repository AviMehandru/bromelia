/* bro-menu-ocr.c */
#include "bro-menu-ocr.h"

#include "bro-cell-ref.h"
#include "bro-menu-numbers.h"
#include "bro-message-code.h"

#define SECTOR G_GINT64_CONSTANT (2048)

struct _BroMenuOcr {
  GObject parent_instance;
  BroProcessLauncher *launcher;
  BroToolLocator *locator;
  BroFileSystem *fs;
};

G_DEFINE_FINAL_TYPE (BroMenuOcr, bro_menu_ocr, G_TYPE_OBJECT)

static void
stop_process (gpointer process, gpointer unused)
{
  bro_running_process_stop (process, BRO_STOP_REASON_CANCELLED);
}

static char *
tool (BroMenuOcr *self, BroToolKind kind, BroBroError **error)
{
  g_autoptr (BroToolInfo) info = bro_tool_locator_locate (self->locator, kind);
  if (info->path)
    return g_strdup (info->path);
  if (error)
    {
      if (info->why)
        *error = bro_bro_message_to_error (info->why, NULL);
      else
        {
          BroJsonValue *params = bro_json_value_new_object ();
          bro_json_value_set (params, "tool", bro_json_value_new_string (bro_tool_kind_to_wire (kind)));
          bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_TOOL_MISSING), params);
        }
    }
  return NULL;
}

/* Runs @exe; its stdout joined by newlines goes to *@text (when not NULL). FALSE and @error set when it can't run or
 * was cancelled. */
static gboolean
run (BroMenuOcr *self, const char *exe, const char *const *arguments, BroCancellationToken *cancel, BroProcessExit *exit, char **text,
     BroBroError **error)
{
  g_autoptr (BroProcessSpec) spec = NULL;
  g_autoptr (BroRunningProcess) process = NULL;
  g_autoptr (GString) out = g_string_new (NULL);
  BroOutputLine *line;
  guint handler = 0;
  if (cancel && bro_cancellation_token_is_cancelled (cancel))
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return FALSE;
    }
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (exe);
  g_strfreev (spec->arguments);
  spec->arguments = g_strdupv ((char **) arguments);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  spec->has_stall_timeout = TRUE;
  spec->stall_timeout.seconds = 60;
  process = bro_process_launcher_start (self->launcher, spec, error);
  if (!process)
    return FALSE;
  if (cancel)
    handler = bro_cancellation_token_on_cancel (cancel, stop_process, process, NULL);
  while ((line = bro_running_process_lines (process)) != NULL)
    {
      if (line->stream == BRO_OUTPUT_SOURCE_STDOUT)
        g_string_append_printf (out, "%s%s", out->len ? "\n" : "", line->text);
      bro_output_line_free (line);
    }
  *exit = bro_running_process_wait (process);
  if (handler)
    bro_cancellation_token_disconnect (cancel, handler);
  if (exit->cancelled)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return FALSE;
    }
  if (text)
    *text = g_string_free (g_steal_pointer (&out), FALSE);
  return TRUE;
}

GStrv
bro_menu_ocr_extract_stills (BroMenuOcr *self, BroByteSource *source, GPtrArray *cells, const char *dir, BroCancellationToken *cancel,
                             BroBroError **error)
{
  g_autofree char *ffmpeg = tool (self, BRO_TOOL_KIND_FFMPEG, error);
  g_autoptr (GStrvBuilder) stills = g_strv_builder_new ();
  if (!ffmpeg)
    return NULL;
  for (guint i = 0; cells && i < cells->len; i++)
    {
      BroCellRef *cell = cells->pdata[i];
      g_autofree char *mpg_name = g_strdup_printf ("menu%u.mpg", i), *png_name = g_strdup_printf ("menu%u.png", i);
      g_autofree char *mpg = g_build_filename (dir, mpg_name, NULL), *png = g_build_filename (dir, png_name, NULL);
      g_autoptr (GBytes) bytes = bro_byte_source_read (source, cell->file, cell->first_sector * SECTOR,
                                                       (gsize) ((cell->end_sector - cell->first_sector) * SECTOR));
      const char *args[] = { "-v", "quiet", "-y", "-f", "mpeg", "-i", mpg, "-frames:v", "1", "-vf", "scale=2160:1440,format=gray", png, NULL };
      BroProcessExit exit;
      gboolean ran;
      if (!bro_file_system_write_atomically (self->fs, mpg, bytes, 0644, error))
        return NULL;
      ran = run (self, ffmpeg, args, cancel, &exit, NULL, error);
      bro_file_system_remove (self->fs, mpg, NULL);
      if (!ran)
        return NULL;
      if (exit.status == 0 && bro_file_system_exists (self->fs, png))
        g_strv_builder_add (stills, png);
    }
  return g_strv_builder_end (stills);
}

GArray *
bro_menu_ocr_read_numbers (BroMenuOcr *self, const char *const *stills, BroCancellationToken *cancel, BroBroError **error)
{
  g_autofree char *tesseract = tool (self, BRO_TOOL_KIND_TESSERACT, error);
  g_autoptr (GArray) numbers = NULL;
  if (!tesseract)
    return NULL;
  numbers = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; stills && stills[i]; i++)
    {
      const char *args[] = { stills[i], "stdout", "--psm", "11", NULL };
      g_autofree char *text = NULL;
      g_autoptr (GArray) found = NULL;
      BroProcessExit exit;
      if (!run (self, tesseract, args, cancel, &exit, &text, error))
        return NULL;
      if (exit.status != 0)
        continue;
      found = bro_menu_numbers_parse (text);
      for (guint k = 0; k < found->len; k++)
        {
          int n = g_array_index (found, int, k);
          gboolean seen = FALSE;
          for (guint j = 0; j < numbers->len && !seen; j++)
            seen = g_array_index (numbers, int, j) == n;
          if (!seen)
            g_array_append_val (numbers, n);
        }
    }
  return g_steal_pointer (&numbers);
}

static void
bro_menu_ocr_finalize (GObject *object)
{
  BroMenuOcr *self = BRO_MENU_OCR (object);
  g_clear_object (&self->launcher);
  g_clear_object (&self->locator);
  g_clear_object (&self->fs);
  G_OBJECT_CLASS (bro_menu_ocr_parent_class)->finalize (object);
}

static void bro_menu_ocr_class_init (BroMenuOcrClass *klass) { G_OBJECT_CLASS (klass)->finalize = bro_menu_ocr_finalize; }
static void bro_menu_ocr_init (BroMenuOcr *self) {}

BroMenuOcr *
bro_menu_ocr_new (BroProcessLauncher *launcher, BroToolLocator *locator, BroFileSystem *fs)
{
  BroMenuOcr *self = g_object_new (BRO_TYPE_MENU_OCR, NULL);
  self->launcher = g_object_ref (launcher);
  self->locator = g_object_ref (locator);
  self->fs = g_object_ref (fs);
  return self;
}
