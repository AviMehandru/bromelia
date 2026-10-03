/* bro-nav-analysis.h: what bro_dvd_nav_analyse found: every title's chapters, how the menus and pre-commands reach
 * chapters, and the menu stills. */
#pragma once

#include "bro-cell-ref.h"
#include "bro-nav-jump.h"
#include "bro-nav-title.h"

G_BEGIN_DECLS

typedef struct {
  GPtrArray *titles; /* BroNavTitle * */
  GPtrArray *jumps;  /* BroNavJump * */
  GPtrArray *stills; /* BroCellRef * */
} BroNavAnalysis;

/* Empty lists. */
BroNavAnalysis *bro_nav_analysis_new (void);
void bro_nav_analysis_free (BroNavAnalysis *analysis);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroNavAnalysis, bro_nav_analysis_free)

G_END_DECLS
