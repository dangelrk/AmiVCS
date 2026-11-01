/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * session.c -- see session.h. g_active_session is the same "single active
 * job, no userdata in the callback signature" bridge pattern
 * apps/amicloud-commander-qt/src/transferjob.cpp uses for
 * UiHooks -- ui_hooks.h's progress hook takes no context pointer, so
 * exactly one VcsGuiSession can have a remote operation in flight at a
 * time per process (true for both frontends: one main window, one repo).
 */
#include "session.h"
#include "vcsname.h"
#include "ui_hooks.h"

#include <stdlib.h>
#include <string.h>

void vcsgui_session_init(VcsGuiSession *s)
{
    memset(s, 0, sizeof(*s));
}

int vcsgui_refresh(VcsGuiSession *s)
{
    VcsStatus st;

    if (!s->is_open) return 0;

    if (!vcs_repo_status_raw(&s->repo, &st)) return 0;
    free(s->staged.items);
    free(s->unstaged.items);
    s->staged = st.staged;
    s->unstaged = st.unstaged;

    vcs_loglist_free(&s->log_all);
    if (!vcs_repo_log_all_raw(&s->repo, &s->log_all)) return 0;

    vcsgui_graph_layout_free(&s->graph);
    if (!vcsgui_compute_graph_layout(&s->log_all, &s->graph)) return 0;

    vcs_head_load(s->repo.amivcs_dir, &s->head);

    vcs_refs_free(&s->branches);
    if (!vcs_refs_load(s->repo.amivcs_dir, &s->branches)) return 0;

    return 1;
}

int vcsgui_open(VcsGuiSession *s, const char *root)
{
    strncpy(s->root, root, sizeof(s->root) - 1);
    s->root[sizeof(s->root) - 1] = '\0';

    if (!vcs_repo_open(&s->repo, root)) { s->is_open = 0; return 0; }
    s->is_open = 1;
    return vcsgui_refresh(s);
}

int vcsgui_init_repo(VcsGuiSession *s, const char *root)
{
    if (!vcs_repo_init(root)) return 0;
    return vcsgui_open(s, root);
}

void vcsgui_close(VcsGuiSession *s)
{
    free(s->staged.items);
    free(s->unstaged.items);
    vcs_loglist_free(&s->log_all);
    vcsgui_graph_layout_free(&s->graph);
    vcs_refs_free(&s->branches);
    memset(s, 0, sizeof(*s));
}

void vcsgui_set_author(VcsGuiSession *s, const char *author)
{
    vcs_utf8_copy(s->author, sizeof(s->author), author ? author : "");   /* whole characters */
}

int vcsgui_add(VcsGuiSession *s, const char *relpath)
{
    if (!vcs_index_add(&s->repo, relpath)) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_remove(VcsGuiSession *s, const char *relpath)
{
    if (!vcs_index_remove(&s->repo, relpath)) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_add_all(VcsGuiSession *s)
{
    if (!vcs_index_add_all(&s->repo)) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_commit(VcsGuiSession *s, const char *message, int stage_all, VcsOid *out_oid)
{
    if (!vcs_repo_commit(&s->repo, s->author, message, stage_all, out_oid)) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_branch_create(VcsGuiSession *s, const char *name)
{
    if (!vcs_repo_branch(&s->repo, name)) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_switch(VcsGuiSession *s, const char *branch)
{
    if (!vcs_repo_switch(&s->repo, branch)) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_checkout(VcsGuiSession *s)
{
    if (!vcs_repo_checkout(&s->repo)) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_merge(VcsGuiSession *s, const char *branch)
{
    if (!vcs_repo_merge(&s->repo, branch)) return 0;
    return vcsgui_refresh(s);
}

/* ------------------------------------------------------------------ */
/* Remote ops + progress/cancel bridge. */

static VcsGuiSession *g_active_session = NULL;

static int session_progress_bridge(long done, long total, const char *name)
{
    VcsGuiSession *s = g_active_session;
    if (!s) return 1;
    if (s->cancel_requested) return 0;
    if (s->progress_cb) return s->progress_cb(s->progress_userdata, done, total, name);
    return 1;
}

/* Last message repo.c/merge.c/remote.c reported via ui_notify(), captured
 * rather than shown here -- a frontend decides for itself whether/how to
 * display it (message box, status bar, ...), see vcsgui_last_message(). */
static char g_last_message[1024] = "";

static void session_notify_bridge(int level, const char *msg)
{
    (void)level;
    strncpy(g_last_message, msg, sizeof(g_last_message) - 1);
    g_last_message[sizeof(g_last_message) - 1] = '\0';
}

void vcsgui_install_hooks(void)
{
    UiHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.progress = session_progress_bridge;
    hooks.notify = session_notify_bridge;
    ui_set_hooks(&hooks);
}

const char *vcsgui_last_message(void)
{
    return g_last_message;
}

void vcsgui_request_cancel(VcsGuiSession *s)
{
    s->cancel_requested = 1;
}

int vcsgui_clone(VcsGuiSession *s, const AcConfig *cfg, const char *repo_name, const char *local_dir)
{
    int ok;

    g_active_session = s;
    s->cancel_requested = 0;
    ok = vcs_remote_clone(cfg, repo_name, local_dir);
    g_active_session = NULL;

    if (!ok) return 0;
    return vcsgui_open(s, local_dir);
}

int vcsgui_push(VcsGuiSession *s, const AcConfig *cfg, const char *repo_name)
{
    int ok;

    if (!s->is_open) return 0;
    g_active_session = s;
    s->cancel_requested = 0;
    ok = vcs_remote_push(&s->repo, cfg, repo_name);
    g_active_session = NULL;

    if (!ok) return 0;
    return vcsgui_refresh(s);
}

int vcsgui_pull(VcsGuiSession *s, const AcConfig *cfg, const char *repo_name, const char *branch)
{
    int ok;

    if (!s->is_open) return 0;
    g_active_session = s;
    s->cancel_requested = 0;
    ok = vcs_remote_pull(&s->repo, cfg, repo_name, branch);
    g_active_session = NULL;

    if (!ok) return 0;
    return vcsgui_refresh(s);
}
