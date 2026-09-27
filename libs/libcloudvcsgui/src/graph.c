/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * graph.c -- see graph.h. Lane assignment is tracked as a growable array
 * of "what OID is this lane waiting for" (VCS_OID_ZERO = free) -- the
 * same small-scale-friendly dynamic-array-instead-of-hash-table idiom
 * libcloudvcs's VcsOidSet uses.
 */
#include "graph.h"

#include <stdlib.h>

typedef struct {
    VcsOid *waiting; /* one entry per lane; VCS_OID_ZERO = free */
    int n, cap;
} LaneSet;

static void lanes_init(LaneSet *ls) { ls->waiting = NULL; ls->n = ls->cap = 0; }
static void lanes_free(LaneSet *ls) { free(ls->waiting); ls->waiting = NULL; ls->n = ls->cap = 0; }

static int lanes_find(const LaneSet *ls, const VcsOid *oid)
{
    int i;
    for (i = 0; i < ls->n; i++) {
        if (vcs_oid_equal(&ls->waiting[i], oid)) return i;
    }
    return -1;
}

static int lanes_alloc(LaneSet *ls)
{
    int i;
    for (i = 0; i < ls->n; i++) {
        if (vcs_oid_is_zero(&ls->waiting[i])) return i;
    }

    if (ls->n == ls->cap) {
        int newcap = ls->cap ? ls->cap * 2 : 8;
        VcsOid *tmp = (VcsOid *)realloc(ls->waiting, sizeof(VcsOid) * (size_t)newcap);
        if (!tmp) return -1;
        ls->waiting = tmp;
        ls->cap = newcap;
    }
    ls->waiting[ls->n] = VCS_OID_ZERO;
    return ls->n++;
}

int vcsgui_compute_graph_layout(const VcsLogList *log, VcsGraphLayout *out)
{
    LaneSet lanes;
    int i;

    out->items = NULL;
    out->n = 0;
    out->max_lanes = 0;

    if (log->n == 0) return 1;

    out->items = (VcsGraphRow *)malloc(sizeof(VcsGraphRow) * (size_t)log->n);
    if (!out->items) return 0;
    out->n = log->n;

    lanes_init(&lanes);

    for (i = 0; i < log->n; i++) {
        const VcsLogEntry *entry = &log->items[i];
        VcsGraphRow *row = &out->items[i];
        int primary, j;

        primary = lanes_find(&lanes, &entry->oid);
        if (primary < 0) primary = lanes_alloc(&lanes);
        if (primary < 0) { lanes_free(&lanes); out->n = i; vcsgui_graph_layout_free(out); return 0; }

        /* Two tips converging on the same commit: collapse the extra
         * lane(s) into `primary` (see graph.h). */
        for (j = 0; j < lanes.n; j++) {
            if (j != primary && vcs_oid_equal(&lanes.waiting[j], &entry->oid)) lanes.waiting[j] = VCS_OID_ZERO;
        }

        /* Snapshot every currently-live lane (primary included, even if
         * lanes_alloc() just handed it back zeroed -- it's this row's own
         * commit either way) as "passes through this row" for the
         * renderer (see graph.h's active_lanes doc). */
        {
            int cnt = 0;
            for (j = 0; j < lanes.n; j++) {
                if (j == primary || !vcs_oid_is_zero(&lanes.waiting[j])) cnt++;
            }
            row->active_lanes = (int *)malloc(sizeof(int) * (size_t)(cnt > 0 ? cnt : 1));
            row->nactive_lanes = 0;
            if (row->active_lanes) {
                for (j = 0; j < lanes.n; j++) {
                    if (j == primary || !vcs_oid_is_zero(&lanes.waiting[j])) row->active_lanes[row->nactive_lanes++] = j;
                }
            }
        }

        row->lane = primary;
        row->nparent_lanes = 0;

        if (entry->nparents == 0) {
            lanes.waiting[primary] = VCS_OID_ZERO; /* root commit: free the lane */
        } else {
            int k;

            lanes.waiting[primary] = entry->parents[0];
            row->parent_lanes[row->nparent_lanes++] = primary;

            for (k = 1; k < entry->nparents; k++) {
                int plane = lanes_find(&lanes, &entry->parents[k]);
                if (plane < 0) {
                    plane = lanes_alloc(&lanes);
                    if (plane < 0) { lanes_free(&lanes); out->n = i + 1; vcsgui_graph_layout_free(out); return 0; }
                    lanes.waiting[plane] = entry->parents[k];
                }
                row->parent_lanes[row->nparent_lanes++] = plane;
            }
        }

        if (lanes.n > out->max_lanes) out->max_lanes = lanes.n;
    }

    lanes_free(&lanes);
    return 1;
}

void vcsgui_graph_layout_free(VcsGraphLayout *out)
{
    int i;
    for (i = 0; i < out->n; i++) free(out->items[i].active_lanes);
    free(out->items);
    out->items = NULL;
    out->n = 0;
    out->max_lanes = 0;
}
