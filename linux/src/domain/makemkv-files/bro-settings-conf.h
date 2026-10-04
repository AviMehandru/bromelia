/* bro-settings-conf.h: BroSettingsConf, MakeMKV's settings.conf format: key = "value" lines, # comments. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Every key = value line (char * → char *; quotes around the value removed); the last of a repeated key wins. */
GHashTable *bro_settings_conf_parse (const char *text);

/* A header naming @header, then the keys of @settings (char * → char *) in code point order; a double quote in a
 * value becomes a single one and a line break a space. */
char *bro_settings_conf_render (GHashTable *settings, const char *header);

G_END_DECLS
