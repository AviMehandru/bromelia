/* bro-token-registry.h: BroTokenRegistry, the values of the template tokens. */
#pragma once

#include "bro-identity.h"
#include "bro-title.h"
#include "bro-token-context.h"

G_BEGIN_DECLS

/* The tokens shared by every file of a job (char * → char *): name, kind, format, rip, discLabel, discNumber,
 * season, part, volumeNumber, disc, volume, type, drive, job, date, time, year, month, day, releaseYear, seasonOr1
 * and libraryFolder (Movies / TV Shows). The per-file tokens (episode, episodeNumber, episodeTitle, track) are
 * empty here; each BroPlannedOutput sets its own. */
GHashTable *bro_token_registry_values (const BroIdentity *identity, const BroTokenContext *context);

/* "Title 11" for DVD titles, "Playlist 00800" for Blu-ray playlists, else MakeMKV's title number. */
char *bro_token_registry_track_label (const BroTitle *title);

/* "Episode 007" for episode 7 at width 3. */
char *bro_token_registry_episode_label (int episode, int width);

G_END_DECLS
