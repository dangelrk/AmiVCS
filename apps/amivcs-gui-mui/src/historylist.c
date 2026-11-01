/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * historylist.c — see historylist.h.
 */
#include <libraries/mui.h>

#include "historylist.h"
#include "vcsname.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void historylist_build(VcsGuiSession *session, HistoryRow **out_rows, int *out_n)
{
    int i;
    HistoryRow *rows;

    *out_rows = NULL;
    *out_n = 0;
    if (session->log_all.n == 0) return;

    rows = (HistoryRow *)malloc(sizeof(HistoryRow) * (size_t)session->log_all.n);
    if (!rows) return;

    for (i = 0; i < session->log_all.n; i++) {
        rows[i].entry = &session->log_all.items[i];
        rows[i].row = &session->graph.items[i];
        rows[i].max_lanes = session->graph.max_lanes;
    }
    *out_rows = rows;
    *out_n = session->log_all.n;
}

void historylist_free(HistoryRow *rows)
{
    free(rows);
}

/* Static row buffers -- MUI renders one row at a time and copies the
 * strings out before the next hook call (same documented idiom
 * panel_mui.c's panel_display_func() already relies on). */
static char g_disp_graph[64];
static char g_disp_oid[16];
static char g_disp_date[24];
static char g_disp_msg[300];
static char g_disp_author[64];

static __saveds LONG history_display_func(__reg("a0") struct Hook *hook,
                                          __reg("a2") char **array,
                                          __reg("a1") HistoryRow *hr)
{
    (void)hook;

    if (!hr) {
        array[0] = "Graph";
        array[1] = "OID";
        array[2] = "Author";
        array[3] = "Date";
        array[4] = "Message";
        return 0;
    }

    {
        int lane, pos = 0;
        for (lane = 0; lane < hr->max_lanes && pos + 2 < (int)sizeof(g_disp_graph); lane++) {
            int active = 0, k;
            for (k = 0; k < hr->row->nactive_lanes; k++) {
                if (hr->row->active_lanes[k] == lane) { active = 1; break; }
            }
            g_disp_graph[pos++] = (lane == hr->row->lane) ? '*' : (active ? '|' : ' ');
            g_disp_graph[pos++] = ' ';
        }
        g_disp_graph[pos] = '\0';
    }
    array[0] = g_disp_graph;

    {
        char hex[VCS_OID_HEX];
        vcs_oid_to_hex(&hr->entry->oid, hex);
        strncpy(g_disp_oid, hex, 8);
        g_disp_oid[8] = '\0';
    }
    array[1] = g_disp_oid;

    vcs_text_to_local_buf(hr->entry->author, g_disp_author, sizeof(g_disp_author));
    array[2] = g_disp_author;

    {
        time_t t = (time_t)hr->entry->mtime;
        struct tm *tm = gmtime(&t);
        if (tm) {
            sprintf(g_disp_date, "%04d-%02d-%02d %02d:%02d",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
        } else {
            g_disp_date[0] = '\0';
        }
    }
    array[3] = g_disp_date;

    {
        char msg[sizeof(g_disp_msg)];
        vcs_text_to_local_buf(hr->entry->message ? hr->entry->message : "", msg, sizeof(msg));
        if (hr->entry->decoration[0])
            snprintf(g_disp_msg, sizeof(g_disp_msg), "[%s] %s", hr->entry->decoration, msg);
        else {
            strncpy(g_disp_msg, msg, sizeof(g_disp_msg) - 1);
            g_disp_msg[sizeof(g_disp_msg) - 1] = '\0';
        }
    }
    array[4] = g_disp_msg;

    return 0;
}

struct Hook history_display_hook = { { NULL, NULL }, (APTR)history_display_func, NULL, NULL };
