/* bro-identity-private.h: helpers shared by Domain modules (internal in Swift and C# too). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Lower case, letters and digits only: "Kauboi-Bibappu!" → "kauboibibappu". Per Unicode character, with simple
 * lower-case mappings, as on the other platforms. Free with g_free. */
char *_bro_identity_normalize_name (const char *name);

G_END_DECLS
