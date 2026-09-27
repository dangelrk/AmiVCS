/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCSGUI_GRAPH_H
#define CLOUDVCSGUI_GRAPH_H

/*
 * graph.h -- commit-graph lane layout (AMIVCS-GUI plan v2 section 2):
 * turns a VcsLogList (libcloudvcs's repo.h, vcs_repo_log_all_raw()'s
 * newest-first, decorated, full-DAG commit list) into a lane/column
 * assignment per row -- the one shape both frontends render from: Qt
 * paints it with QPainter, MUI composes it into an ASCII-art column
 * ("| * ", "|/  ", ...) for MUIA_List_DisplayHook. Pure presentation
 * layout, no VCS decision-making, so it lives here rather than in
 * libcloudvcs (module-encapsulation rule).
 *
 * Algorithm: a single top-down pass over log (which must already be
 * newest-first, as vcs_repo_log_all_raw() produces it). One "active
 * lane" is kept per branch tip currently being walked, each waiting for
 * a specific parent OID; a commit takes over whichever lane was waiting
 * for it (or opens a fresh one, for a not-yet-seen tip), then hands its
 * own lane down to its first parent and -- for a merge commit -- opens
 * a second lane for its second parent. A lane whose commit turns out to
 * be a root (no parents) is freed and can be reused by a later,
 * unrelated branch, so the graph doesn't grow unboundedly wide. If two
 * tips happen to converge on the very same commit, the lower-numbered
 * lane claims the row and the other is simply freed into that column
 * rather than drawing a separate merge line -- a deliberate v1
 * simplification (this is not a Git clone).
 */

#include "repo.h"

typedef struct {
    int lane;                          /* this row's own column */
    int parent_lanes[VCS_MAX_PARENTS]; /* column(s) the line(s) down to its parent(s) go to */
    int nparent_lanes;
    int *active_lanes;                 /* every column with a line passing through this row
                                         * (this row's own lane always included), sorted
                                         * ascending -- draw a plain vertical segment for each
                                         * one that ISN'T `lane` itself; malloc'd, freed by
                                         * vcsgui_graph_layout_free(). Needed because a commit's
                                         * actual parent row is often not the very next row
                                         * (other branches interleave by date in between) -- a
                                         * renderer using only `lane`/`parent_lanes` would draw
                                         * dots and short local connectors but no continuous
                                         * line for the columns just passing through. */
    int nactive_lanes;
} VcsGraphRow;

typedef struct {
    VcsGraphRow *items; /* one per VcsLogList entry, same order/index */
    int n;
    int max_lanes;      /* widest column index used + 1 -- how many columns to render */
} VcsGraphLayout;

/* log must already be newest-first (vcs_repo_log_all_raw()'s order) --
 * an older-first list produces a nonsensical layout, this doesn't sort.
 * Returns 1 on success, 0 only on allocation failure. */
int  vcsgui_compute_graph_layout(const VcsLogList *log, VcsGraphLayout *out);
void vcsgui_graph_layout_free(VcsGraphLayout *out);

#endif /* CLOUDVCSGUI_GRAPH_H */
