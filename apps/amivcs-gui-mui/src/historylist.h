/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef HISTORYLIST_H
#define HISTORYLIST_H

/*
 * historylist.h — MUI list rows for the history view: pairs each
 * VcsLogEntry with its VcsGraphRow (libcloudvcsgui's graph.h) so the
 * display hook has both without needing the row's array index (MUI's
 * DisplayHook only ever gets the entry pointer that was inserted).
 * Column 0's text is a small ASCII-art rendering of the graph -- the
 * same VcsGraphLayout data the Qt frontend instead paints with a
 * QStyledItemDelegate (apps/amivcs-gui-qt/src/graphdelegate.cpp).
 */

#include "session.h"

typedef struct {
    const VcsLogEntry *entry;
    const VcsGraphRow *row;
    int max_lanes;
} HistoryRow;

/* Builds one HistoryRow per session->log_all entry (malloc'd array,
 * caller frees with historylist_free()). Pointers borrow session's
 * storage -- do not call vcsgui_refresh()/vcsgui_close() on the session
 * while a HistoryRow array built from it is still in use. */
void historylist_build(VcsGuiSession *session, HistoryRow **out_rows, int *out_n);
void historylist_free(HistoryRow *rows);

/* MUIA_List_DisplayHook for a 5-column list (Graph, OID, Author, Date,
 * Message) fed HistoryRow pointers via MUIM_List_InsertSingle. */
extern struct Hook history_display_hook;

#endif /* HISTORYLIST_H */
