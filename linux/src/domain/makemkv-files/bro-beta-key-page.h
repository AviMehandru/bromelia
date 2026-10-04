/* bro-beta-key-page.h: BroBetaKeyPage, the forum page where MakeMKV's developer posts the free beta key. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define BRO_BETA_KEY_PAGE_URL "https://forum.makemkv.com/forum/viewtopic.php?f=5&t=1053"

/* The key in the post (the T-… string in its code block, else the first one on the page); NULL when there is none. */
char *bro_beta_key_page_parse (const char *html);

G_END_DECLS
