/* bro-one-pass.h: BroOnePass, ripping chosen titles in one makemkvcon run. "mkv" takes one title or "all", so a
 * subset can only be ripped in one run when a minimum title length leaves exactly that subset. */
#pragma once

#include "bro-listing.h"
#include "bro-one-pass-plan.h"

G_BEGIN_DECLS

/* The minimum length that keeps the chosen titles (@indices, int) and drops the others; FALSE to rip title by
 * title. Listed lengths are rounded to seconds, so the longest title left out must be at least 2 s shorter than
 * the shortest chosen; the length must also be longer than @current_min_length (-1: none). */
gboolean bro_one_pass_plan (GArray *indices, const BroListing *listing, int current_min_length, BroOnePassPlan *out);

/* Whether @listing (read with that minimum length) holds exactly the chosen titles (@chosen, int) of @original. */
gboolean bro_one_pass_matches (const BroListing *listing, GArray *chosen, const BroListing *original);

G_END_DECLS
