/* bro-dvd-nav.h: BroDvdNav, finds the episodes inside a DVD "play all" title by reading the disc's own navigation:
 * the title tables in the IFO files and the jump commands of menus, buttons and title pre-commands (was bro-dvd.c). */
#pragma once

#include "bro-byte-source.h"
#include "bro-chapter-range.h"
#include "bro-episode-plan.h"
#include "bro-jump.h"
#include "bro-nav-analysis.h"

G_BEGIN_DECLS

/* The titles, chapter jumps and menu stills of the VIDEO_TS in @source; NULL when VIDEO_TS.IFO isn't a video
 * manager. */
BroNavAnalysis *bro_dvd_nav_analyse (BroByteSource *source);

/* Plans (BroEpisodePlan *) for every title that the menus jump into at two or more chapters, most episodes first. */
GPtrArray *bro_dvd_nav_plans (const BroNavAnalysis *analysis);

/* The episodes of title @title_index: an episode starts at every chapter a menu, button or pre-command jumps to, and
 * at chapter 1. The last one ends at the next VOB boundary (or, in a single VOB, after as many chapters as the one
 * before it). NULL without at least two starts. */
BroEpisodePlan *bro_dvd_nav_episode_plan (const BroNavAnalysis *analysis, int title_index);

/* The menu stills (BroCellRef *, borrowed from @analysis), one per VOB/cell id: input for MenuOcr. */
GPtrArray *bro_dvd_nav_still_cells (const BroNavAnalysis *analysis);

/* An 8-byte VM command: LinkPTTN, JumpVTS_PTT or JumpTT with its condition; NULL for any other. */
BroJump *bro_dvd_nav_decode_jump (const guint8 *bytes, gsize length);

/* The MKV chapters (int) to split at for the disc chapters @split (int), by the MKV's chapter starts @mkv_starts
 * (double; they may begin with an added chapter 00); a copy of @split when the starts are unknown (NULL or empty);
 * NULL when a chapter has no MKV chapter within 1 s. */
GArray *bro_dvd_nav_mkv_chapters (GArray *split, const BroEpisodePlan *plan, GArray *mkv_starts);

/* The disc chapters of episode @episode (0-based). */
BroChapterRange bro_dvd_nav_chapter_range (const BroEpisodePlan *plan, int episode);

/* Whether the episodes look like real episodes rather than a movie's scene selection: strict, at least 3 of 15
 * minutes or more within 35 % of each other; otherwise at least 2 of 5 minutes or more within a factor of 2. */
gboolean bro_dvd_nav_is_plausible (const BroEpisodePlan *plan, gboolean strict);

/* Whether a ripped title with @chapters chapters is this plan's title (MakeMKV may drop one short chapter). */
gboolean bro_dvd_nav_matches_chapter_count (const BroEpisodePlan *plan, int chapters);

G_END_DECLS
