/* bro-config-templates.h: BroConfigTemplates, ready-made profiles. */
#pragma once

#include "bro-profile.h"

G_BEGIN_DECLS

/* Archive everything (today's preset, now a profile any drive can use): a decrypted backup then MKV of every title,
 * keeping the backup and the unsplit play-all title, with every archive file. Without an id: the caller gives it
 * one when adding it. */
BroProfile *bro_config_templates_archive_everything (void);

G_END_DECLS
