/* bro-nav-analysis.c */
#include "bro-nav-analysis.h"

BroNavAnalysis *
bro_nav_analysis_new (void)
{
  BroNavAnalysis *a = g_new0 (BroNavAnalysis, 1);
  a->titles = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_nav_title_free);
  a->jumps = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_nav_jump_free);
  a->stills = g_ptr_array_new_with_free_func ((GDestroyNotify) bro_cell_ref_free);
  return a;
}

void
bro_nav_analysis_free (BroNavAnalysis *analysis)
{
  if (!analysis)
    return;
  g_ptr_array_unref (analysis->titles);
  g_ptr_array_unref (analysis->jumps);
  g_ptr_array_unref (analysis->stills);
  g_free (analysis);
}
