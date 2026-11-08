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
#include "vcsconfig.h"
#include "remote.h"
#include <stdio.h>

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
    vcs_mergestate_free(&s->merge);
    s->staged = st.staged;
    s->unstaged = st.unstaged;
    s->merge = st.merge;   /* moved: st is not freed */

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
    vcs_mergestate_free(&s->merge);
    vcs_loglist_free(&s->log_all);
    vcsgui_graph_layout_free(&s->graph);
    vcs_refs_free(&s->branches);
    memset(s, 0, sizeof(*s));
}

void vcsgui_set_author(VcsGuiSession *s, const char *author)
{
    vcs_utf8_copy(s->author, sizeof(s->author), author ? author : "");   /* whole characters */
}

/* writing operations hold .amivcs/lock (repo.h), so the CLI and the IDE
 * never change the repository at the same time */
#define LOCKED(call) do { int ok_; \
        if (!vcs_repo_lock(s->repo.amivcs_dir)) return 0; \
        ok_ = (call); \
        vcs_repo_unlock(s->repo.amivcs_dir); \
        if (!ok_) return 0; } while (0)

int vcsgui_add(VcsGuiSession *s, const char *relpath)
{
    LOCKED(vcs_index_add(&s->repo, relpath));
    return vcsgui_refresh(s);
}

int vcsgui_remove(VcsGuiSession *s, const char *relpath)
{
    LOCKED(vcs_index_unstage(&s->repo, relpath));   /* back to HEAD, not "stage a deletion" */
    return vcsgui_refresh(s);
}

int vcsgui_add_all(VcsGuiSession *s)
{
    LOCKED(vcs_index_add_all(&s->repo));
    return vcsgui_refresh(s);
}

int vcsgui_identity(VcsGuiSession *s, char *out, size_t sz)
{
    char local[300];
    if (s->author[0]) { vcs_utf8_copy(out, sz, s->author); return 1; }
    if (!vcs_identity(s->is_open ? s->repo.amivcs_dir : NULL, local, sizeof(local))) {
        if (sz) out[0] = '\0';
        return 0;
    }
    {
        char *u = vcs_text_from_local(local);   /* commit text is UTF-8 */
        vcs_utf8_copy(out, sz, u ? u : local);
        free(u);
    }
    return 1;
}

int vcsgui_commit(VcsGuiSession *s, const char *message, int stage_all, VcsOid *out_oid)
{
    char author[300];
    if (!vcsgui_identity(s, author, sizeof(author))) {
        ui_notify(UI_ERROR, "amivcs: no author name set -- set name and email first");
        return 0;
    }
    LOCKED(vcs_repo_commit(&s->repo, author, message, stage_all, out_oid));
    return vcsgui_refresh(s);
}

int vcsgui_branch_create(VcsGuiSession *s, const char *name)
{
    LOCKED(vcs_repo_branch(&s->repo, name));
    return vcsgui_refresh(s);
}

int vcsgui_switch(VcsGuiSession *s, const char *branch)
{
    LOCKED(vcs_repo_switch(&s->repo, branch));
    return vcsgui_refresh(s);
}

int vcsgui_checkout(VcsGuiSession *s)
{
    LOCKED(vcs_repo_checkout(&s->repo));
    return vcsgui_refresh(s);
}

int vcsgui_merge(VcsGuiSession *s, const char *branch)
{
    LOCKED(vcs_repo_merge(&s->repo, branch));
    return vcsgui_refresh(s);
}

int vcsgui_merge_abort(VcsGuiSession *s)
{
    LOCKED(vcs_repo_merge_abort(&s->repo));
    return vcsgui_refresh(s);
}

int vcsgui_resolve(VcsGuiSession *s, const char *relpath, int take_theirs)
{
    LOCKED(vcs_repo_merge_resolve(&s->repo, relpath, take_theirs));
    return vcsgui_refresh(s);
}

int vcsgui_conflicts_open(const VcsGuiSession *s)
{
    return s->merge.active ? vcs_mergestate_unresolved(&s->merge) : 0;
}

int vcsgui_diff_file(VcsGuiSession *s, const char *relpath, int staged,
                     const VcsRepoDiffSink *sink, void *ctx)
{
    VcsDiffSide from, to;
    const char *paths[1];
    if (!s->is_open) return -1;
    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));
    if (staged) {
        from.kind = VCS_SIDE_COMMIT;
        if (!vcs_head_resolve(s->repo.amivcs_dir, &from.oid)) from.oid = VCS_OID_ZERO;
        to.kind = VCS_SIDE_INDEX;
    } else {
        from.kind = VCS_SIDE_INDEX;
        to.kind = VCS_SIDE_WORKDIR;
    }
    paths[0] = relpath;
    return vcs_repo_diff(&s->repo, &from, &to, paths, 1, sink, ctx);
}

int vcsgui_diff_commit(VcsGuiSession *s, const VcsOid *commit,
                       const VcsRepoDiffSink *sink, void *ctx)
{
    VcsDiffSide from, to;
    VcsObjType type;
    void *data;
    long len;
    VcsCommit c;
    if (!s->is_open) return -1;
    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));
    from.kind = to.kind = VCS_SIDE_COMMIT;
    from.oid = VCS_OID_ZERO;
    to.oid = *commit;
    if (!vcsobjstore_read(&s->repo.store, commit, &type, &data, &len)) return -1;
    vcs_commit_init(&c);
    if (type == VCS_OBJ_COMMIT && vcs_commit_decode(data, len, &c) && c.nparents > 0)
        from.oid = c.parents[0];
    free(data);
    vcs_commit_free(&c);
    return vcs_repo_diff(&s->repo, &from, &to, NULL, 0, sink, ctx);
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

/* Puts the login into cfg: what the caller gives wins over what the
 * repository remembers (already in cfg from vcs_remote_resolve()). */
static void apply_login(AcConfig *cfg, const VcsGuiLogin *login)
{
    if (!login) return;
    if (login->user[0]) {
        strncpy(cfg->username, login->user, sizeof(cfg->username) - 1);
        cfg->username[sizeof(cfg->username) - 1] = '\0';
    }
    if (login->pass[0]) {
        strncpy(cfg->password, login->pass, sizeof(cfg->password) - 1);
        cfg->password[sizeof(cfg->password) - 1] = '\0';
    }
}

static void remember_login(const char *amivcs_dir, const AcConfig *cfg, const VcsGuiLogin *login)
{
    if (cfg->username[0]) vcs_repo_config_set(amivcs_dir, "user", cfg->username);
    if (login && login->remember && cfg->password[0]) vcs_repo_config_set(amivcs_dir, "pass", cfg->password);
}

int vcsgui_remote_info(VcsGuiSession *s, char *url, size_t usz, char *user, size_t nsz, int *has_pass)
{
    VcsRepoConfig rc;
    if (usz) url[0] = '\0';
    if (nsz) user[0] = '\0';
    if (has_pass) *has_pass = 0;
    if (!s->is_open) return 0;
    vcs_repo_config_load(s->repo.amivcs_dir, &rc);
    strncpy(url, rc.remote, usz - 1); url[usz - 1] = '\0';
    strncpy(user, rc.user, nsz - 1); user[nsz - 1] = '\0';
    if (has_pass) *has_pass = rc.pass[0] != '\0';
    return url[0] != '\0';
}

int vcsgui_auth_failed(void) { return vcs_remote_auth_failed(); }

int vcsgui_clone(VcsGuiSession *s, const AcConfig *opts, const char *url,
                 const VcsGuiLogin *login, const char *local_dir)
{
    AcConfig cfg = *opts;
    char repo_name[256], amivcs_dir[1100];
    int ok;

    if (!vcs_url_split(url, cfg.server, sizeof(cfg.server), repo_name, sizeof(repo_name))) {
        ui_notify(UI_ERROR, "amivcs: '%s' is not a repository URL (https://server/<name>)", url);
        return 0;
    }
    cfg.username[0] = cfg.password[0] = '\0';
    apply_login(&cfg, login);
    g_active_session = s;
    s->cancel_requested = 0;
    ok = vcs_remote_clone(&cfg, repo_name, local_dir);
    g_active_session = NULL;

    if (!ok) return 0;
    snprintf(amivcs_dir, sizeof(amivcs_dir), "%s", local_dir);
    {
        size_t n = strlen(amivcs_dir);
        const char *sep = (n && (amivcs_dir[n - 1] == '/' || amivcs_dir[n - 1] == ':')) ? "" : "/";
        snprintf(amivcs_dir + n, sizeof(amivcs_dir) - n, "%s.amivcs", sep);
    }
    remember_login(amivcs_dir, &cfg, login);
    return vcsgui_open(s, local_dir);
}

/* push and pull share resolving, logging in and remembering */
static int remote_op(VcsGuiSession *s, const AcConfig *opts, const char *url,
                     const VcsGuiLogin *login, int push, const char *branch)
{
    AcConfig cfg = *opts;
    char repo_name[256];
    int ok;

    if (!s->is_open) return 0;
    cfg.username[0] = cfg.password[0] = '\0';
    if (!vcs_remote_resolve(s->repo.amivcs_dir, url, &cfg, repo_name, sizeof(repo_name))) return 0;
    apply_login(&cfg, login);
    g_active_session = s;
    s->cancel_requested = 0;
    if (push) {
        ok = vcs_remote_push(&s->repo, &cfg, repo_name, url && url[0]);
    } else {
        if (!vcs_repo_lock(s->repo.amivcs_dir)) { g_active_session = NULL; return 0; }
        ok = vcs_remote_pull(&s->repo, &cfg, repo_name, branch);
        vcs_repo_unlock(s->repo.amivcs_dir);
    }
    g_active_session = NULL;
    if (!ok) return 0;

    if (url && url[0]) {
        char full[AC_MAX_URL + 256];
        snprintf(full, sizeof(full), "%s/%s", cfg.server, repo_name);
        vcs_repo_config_set(s->repo.amivcs_dir, "remote", full);
    }
    remember_login(s->repo.amivcs_dir, &cfg, login);
    return vcsgui_refresh(s);
}

int vcsgui_push(VcsGuiSession *s, const AcConfig *opts, const char *url, const VcsGuiLogin *login)
{
    return remote_op(s, opts, url, login, 1, NULL);
}

int vcsgui_pull(VcsGuiSession *s, const AcConfig *opts, const char *url, const VcsGuiLogin *login,
                const char *branch)
{
    return remote_op(s, opts, url, login, 0, branch);
}
