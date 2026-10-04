/* bro-makemkv-args.c */
#include "bro-makemkv-args.h"

#include "bro-argument-splitter.h"
#include "bro-bro-message.h"

/* A drive is dev:device when the device is known (stable when drives are renumbered), else disc:N. */
static char *
source_arg (const BroMakemkvSource *s)
{
  switch (s->kind) {
  case BRO_MAKEMKV_SOURCE_DRIVE:
    return *s->device ? g_strconcat ("dev:", s->device, NULL) : g_strdup_printf ("disc:%d", s->index);
  case BRO_MAKEMKV_SOURCE_ISO:
    return g_strconcat ("iso:", s->path, NULL);
  case BRO_MAKEMKV_SOURCE_FILE:
    return g_strconcat ("file:", s->path, NULL);
  }
  return g_strdup ("");
}

static GPtrArray *
common (const BroMakemkvOptions *o)
{
  GPtrArray *a = g_ptr_array_new ();
  g_ptr_array_add (a, g_strdup ("-r"));
  g_ptr_array_add (a, g_strdup ("--progress=-same"));
  g_ptr_array_add (a, g_strdup ("--messages=-stdout"));
  if (!o->scan)
    g_ptr_array_add (a, g_strdup ("--noscan"));
  if (o->profile_path)
    g_ptr_array_add (a, g_strconcat ("--profile=", o->profile_path, NULL));
  if (o->min_length_seconds >= 0)
    g_ptr_array_add (a, g_strdup_printf ("--minlength=%d", o->min_length_seconds));
  if (o->cache_mb > 0)
    g_ptr_array_add (a, g_strdup_printf ("--cache=%d", o->cache_mb));
  if (o->has_direct_io)
    g_ptr_array_add (a, g_strdup (o->direct_io ? "--directio=true" : "--directio=false"));
  g_auto (GStrv) extra = bro_argument_splitter_split (o->extra_arguments ? o->extra_arguments : "");
  for (int i = 0; extra[i]; i++)
    g_ptr_array_add (a, g_strdup (extra[i]));
  return a;
}

static GStrv
finish (GPtrArray *a)
{
  g_ptr_array_add (a, NULL);
  return (GStrv) g_ptr_array_free (a, FALSE);
}

GStrv
bro_makemkv_args_info (const BroMakemkvSource *source, const BroMakemkvOptions *options)
{
  GPtrArray *a = common (options);
  g_ptr_array_add (a, g_strdup ("info"));
  g_ptr_array_add (a, source_arg (source));
  return finish (a);
}

GStrv
bro_makemkv_args_mkv (const BroMakemkvSource *source, const char *title, const char *destination, const BroMakemkvOptions *options)
{
  GPtrArray *a = common (options);
  g_ptr_array_add (a, g_strdup ("mkv"));
  g_ptr_array_add (a, source_arg (source));
  g_ptr_array_add (a, g_strdup (title));
  g_ptr_array_add (a, g_strdup (destination));
  return finish (a);
}

GStrv
bro_makemkv_args_backup (const BroMakemkvSource *source, gboolean decrypt, const char *destination, const BroMakemkvOptions *options,
                         BroBroError **error)
{
  if (source->kind != BRO_MAKEMKV_SOURCE_DRIVE) {
    g_autoptr (BroBroMessage) m = bro_bro_message_new (BRO_MSG_BACKUP_NEEDS_DRIVE, NULL, BRO_SEVERITY_ERROR);
    if (error)
      *error = bro_bro_message_to_error (m, NULL);
    return NULL;
  }
  GPtrArray *a = common (options);
  g_ptr_array_add (a, g_strdup ("backup"));
  if (decrypt)
    g_ptr_array_add (a, g_strdup ("--decrypt"));
  g_ptr_array_add (a, g_strdup_printf ("disc:%d", source->index));
  g_ptr_array_add (a, g_strdup (destination));
  return finish (a);
}

GStrv
bro_makemkv_args_scan_drives (void)
{
  const char *args[] = { "-r", "--cache=1", "info", "disc:9999", NULL };
  return g_strdupv ((char **) args);
}
