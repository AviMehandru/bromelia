/* bro-rip-options-dto.c */
#include "bro-rip-options-dto.h"

BroRipOptionsDto *
bro_rip_options_dto_new (void)
{
  return g_new0 (BroRipOptionsDto, 1);
}

BroRipOptionsDto *
bro_rip_options_dto_copy (const BroRipOptionsDto *options)
{
  BroRipOptionsDto *c = bro_rip_options_dto_new ();
  c->has_mode = options->has_mode;
  c->mode = options->mode;
  c->profile = g_strdup (options->profile);
  c->library = g_strdup (options->library);
  if (options->titles)
    {
      c->titles = g_array_new (FALSE, FALSE, sizeof (int));
      g_array_append_vals (c->titles, options->titles->data, options->titles->len);
    }
  c->output_folder = g_strdup (options->output_folder);
  return c;
}

void
bro_rip_options_dto_free (BroRipOptionsDto *options)
{
  if (!options)
    return;
  g_free (options->profile);
  g_free (options->library);
  g_clear_pointer (&options->titles, g_array_unref);
  g_free (options->output_folder);
  g_free (options);
}
