/* bro-apprise-tool.c */
#include "bro-apprise-tool.h"

#include "bro-apprise-args.h"
#include "bro-message-code.h"
#include "bro-tool-run.h"
#include <string.h>

struct _BroAppriseTool {
  GObject parent_instance;
  BroProcessLauncher *launcher;
  BroToolLocator *locator;
};

G_DEFINE_FINAL_TYPE (BroAppriseTool, bro_apprise_tool, G_TYPE_OBJECT)

char *
_bro_apprise_tool_scheme (const char *url)
{
  const char *sep = strstr (url, "://");
  return sep && sep > url ? g_strndup (url, (gsize) (sep - url)) : g_strdup ("(no scheme)");
}

gboolean
bro_apprise_tool_send (BroAppriseTool *self, const char *url, const char *title, const char *body, BroCancellationToken *cancel,
                       BroBroError **error)
{
  g_autoptr (BroToolInfo) info = bro_tool_locator_locate (self->locator, BRO_TOOL_KIND_APPRISE);
  g_autoptr (BroProcessSpec) spec = NULL;
  BroProcessExit exit;
  if (!info->path)
    {
      g_autofree char *scheme = _bro_apprise_tool_scheme (url);
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "target", bro_json_value_new_string (scheme));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_NOTIFY_NEEDS_APPRISE), params);
      return FALSE;
    }
  spec = bro_process_spec_new ();
  spec->executable = g_strdup (info->path);
  g_strfreev (spec->arguments);
  spec->arguments = bro_apprise_args_build (title, body);
  g_hash_table_unref (spec->environment);
  spec->environment = bro_apprise_args_environment (url);
  spec->stop_policy = BRO_STOP_POLICY_INTERRUPT_FIRST;
  spec->has_stall_timeout = TRUE;
  spec->stall_timeout.seconds = 120;
  if (!bro_tool_run (self->launcher, spec, cancel, NULL, NULL, NULL, &exit, error))
    return FALSE;
  if (exit.cancelled)
    {
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_JOB_CANCELLED), NULL);
      return FALSE;
    }
  if (exit.status != 0)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "status", bro_json_value_new_integer (exit.status));
      bro_bro_error_set (error, bro_message_code_wire (BRO_MSG_NOTIFY_APPRISE_FAILED), params);
      return FALSE;
    }
  return TRUE;
}

static void
bro_apprise_tool_finalize (GObject *object)
{
  BroAppriseTool *self = BRO_APPRISE_TOOL (object);
  g_clear_object (&self->launcher);
  g_clear_object (&self->locator);
  G_OBJECT_CLASS (bro_apprise_tool_parent_class)->finalize (object);
}

static void
bro_apprise_tool_class_init (BroAppriseToolClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = bro_apprise_tool_finalize;
}

static void
bro_apprise_tool_init (BroAppriseTool *self)
{
}

BroAppriseTool *
bro_apprise_tool_new (BroProcessLauncher *launcher, BroToolLocator *locator)
{
  BroAppriseTool *self = g_object_new (BRO_TYPE_APPRISE_TOOL, NULL);
  self->launcher = g_object_ref (launcher);
  self->locator = g_object_ref (locator);
  return self;
}
