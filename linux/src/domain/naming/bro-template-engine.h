/* bro-template-engine.h: BroTemplateEngine, {token} templates for folders, file names and script arguments.
 * {n:3} zero-pads, {token?text} inserts text only when the token is set and not empty, other unknown tokens stay
 * as they are. @values: char * → char *. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

char *bro_template_engine_render (const char *template_text, GHashTable *values);

/* A relative path: values are made safe, '/' (or '\') in the template separates folders, every component is made
 * safe, and empty, "." and ".." components are dropped. Folders are separated by '/' on every platform. */
char *bro_template_engine_render_path (const char *template_text, GHashTable *values);

G_END_DECLS
