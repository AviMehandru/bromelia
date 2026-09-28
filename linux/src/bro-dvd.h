/* bro-dvd.h — finds the episodes inside a DVD "play all" title from the disc's own navigation (IFO title
 * tables and the jump commands of menus, buttons and title pre-commands). Reads an ISO image, a VIDEO_TS
 * folder or a mounted disc. Same analysis as the macOS and Windows versions. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define BRO_DVD_SECTOR 2048

typedef struct _BroVideoTS BroVideoTS;

BroVideoTS *bro_videots_open_iso (const char *path);
/* path may be the VIDEO_TS folder or the folder that contains it. */
BroVideoTS *bro_videots_open_folder (const char *path, const char *label);
void        bro_videots_free (BroVideoTS *r);
const char *bro_videots_label (BroVideoTS *r);

typedef struct {
  GArray *chapters; /* double, seconds */
  GArray *vobs;     /* int, VOB id of each chapter's first cell */
} BroDvdTitle;

typedef struct {
  char *label;
  GHashTable *titles; /* int -> BroDvdTitle* */
  GHashTable *jumps;  /* int title -> GHashTable (int chapter -> char* how) */
} BroDvdAnalysis;

typedef struct {
  int title;
  GArray *starts;         /* int, first chapter (1-based) of each episode */
  int last_end;           /* last chapter of the last episode */
  char *end_rule;
  GArray *tail;           /* int, chapters (>= 1 s) after the last episode */
  GArray *chapter_starts; /* double */
  double duration;        /* without trailing sub-second chapters */
  int kept_chapters;
  GArray *episode_durations; /* double */
  GPtrArray *reasons;        /* char*, how each start is reached */
} BroEpisodePlan;

typedef struct {
  gboolean chapter;  /* TRUE = jump to a chapter, FALSE = to a disc title */
  int title_number;  /* VTS title number for JumpVTS_PTT, -1 for LinkPTTN (current title) */
  int target;        /* chapter or title */
  char *condition;
} BroDvdJump;

gboolean        bro_dvd_decode_jump (const guint8 *c, BroDvdJump *out); /* out->condition must be freed */
BroDvdAnalysis *bro_dvd_analyse (BroVideoTS *r);
void            bro_dvd_analysis_free (BroDvdAnalysis *a);

BroEpisodePlan *bro_dvd_plan (BroDvdAnalysis *a, int title);
GPtrArray      *bro_dvd_plans (BroDvdAnalysis *a); /* BroEpisodePlan*, most episodes first */
void            bro_episode_plan_free (BroEpisodePlan *p);
GArray         *bro_episode_plan_split_chapters (BroEpisodePlan *p); /* int */
void            bro_episode_plan_range (BroEpisodePlan *p, guint i, int *first, int *last);
gboolean        bro_episode_plan_plausible (BroEpisodePlan *p, gboolean strict);
gboolean        bro_episode_plan_matches_chapters (BroEpisodePlan *p, int chapters);

GPtrArray *bro_dvd_menu_stills (BroVideoTS *r); /* GBytes*: MPEG-PS of each menu still, for OCR */
GArray    *bro_dvd_episode_numbers (const char *text); /* int, unique, in order of appearance */
int        bro_dvd_first_episode (GArray *numbers, int count); /* -1 when unclear */
/* Maps disc chapters to MKV chapters with the MKV's chapter start times (NULL = identity). NULL when they don't line up. */
GArray    *bro_dvd_mkv_chapters (GArray *disc_chapters, BroEpisodePlan *p, GArray *mkv_starts);
char      *bro_dvd_hms (double seconds);
GArray    *bro_parse_simple_chapters (const char *text); /* double */

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroVideoTS, bro_videots_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroDvdAnalysis, bro_dvd_analysis_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroEpisodePlan, bro_episode_plan_free)

G_END_DECLS
