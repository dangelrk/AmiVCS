/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCSGUI_SESSION_H
#define CLOUDVCSGUI_SESSION_H

/*
 * session.h -- VcsGuiSession: the headless controller both amivcs-gui-qt
 * and amivcs-gui-mui drive (AMIVCS-GUI plan, deliberately in
 * libs/libcloudvcsgui/ rather than core/common/ like the existing
 * commander apps' shared panel.c -- an explicit deviation from that
 * precedent). Pure C, no Qt/MUI types anywhere in this header or its
 * implementation -- each frontend owns 100% of its own widgets/painting
 * and calls into this for everything VCS-shaped.
 *
 * Contract: a session holds cached, refreshable view state (staged/
 * unstaged change lists, full multi-branch history + graph layout,
 * current HEAD, branch list) alongside the open VcsRepo itself, so a
 * frontend can just re-render its widgets from session fields after any
 * mutating call instead of re-querying libcloudvcs by hand. Every
 * mutating vcsgui_*() call below re-runs vcsgui_refresh() on success, so
 * the session's fields are always current after a call returns 1.
 *
 * If vcsgui_open() fails because root isn't a repository yet (not an
 * I/O error -- vcs_repo_open() simply found no .amivcs/), the frontend
 * is expected to offer "Init repository here" and call
 * vcsgui_init_repo() rather than just reporting an error (a v1 UX
 * decision, not something this header can detect/distinguish itself --
 * both failure modes return 0 the same way vcs_repo_open() does).
 */

#include "repo.h"
#include "index.h"
#include "merge.h"
#include "remote.h"
#include "graph.h"
#include "amicloud.h"

typedef struct {
    VcsRepo repo;
    int     is_open;       /* 1 once repo holds a successfully opened repository */
    char    root[1024];    /* directory the session was opened against */
    char    author[64];    /* commit identity for vcsgui_commit(); see vcsgui_set_author() */

    VcsHead     head;       /* current HEAD (attached branch or detached oid) */
    VcsRefList  branches;   /* every local branch */

    VcsChangeList  staged, unstaged;   /* vcs_repo_status_raw()'s split */
    VcsLogList     log_all;            /* vcs_repo_log_all_raw()'s full multi-branch history */
    VcsGraphLayout graph;               /* vcsgui_compute_graph_layout() over log_all */

    /* Set by a running vcsgui_clone()/push()/pull() (see below); a
     * frontend's Cancel button calls vcsgui_request_cancel() from
     * whatever thread it runs on -- reading/writing a single int is safe
     * without a lock on every platform this targets (68000 included, no
     * true SMP), matching apps/amicloud-commander-qt's
     * TransferJob::m_cancelRequested's use of a plain flag. */
    volatile int cancel_requested;

    /* Optional: forwards each ui_progress() call made during
     * vcsgui_clone()/push()/pull() (see vcsgui_install_progress_hook()).
     * NULL = no progress reporting, transfers just run to completion or
     * cancellation. Return 0 to cancel (same convention as
     * UiHooks.progress), same as returning nonzero otherwise. */
    int (*progress_cb)(void *userdata, long done, long total, const char *label);
    void *progress_userdata;
} VcsGuiSession;

/* Zeroes *s. Call once before the session's first vcsgui_open()/
 * vcsgui_init_repo() -- NOT before every open (that would leak the
 * previous repo's cached lists); call vcsgui_close() first if reusing a
 * session for a different repository. */
void vcsgui_session_init(VcsGuiSession *s);

/* Opens an existing repository at root and populates every cached field
 * via vcsgui_refresh(). Returns 1 on success, 0 if root isn't an AMIVCS
 * repository (see this header's file comment) or on I/O error. */
int vcsgui_open(VcsGuiSession *s, const char *root);

/* Creates a new repository at root (vcs_repo_init()) then opens it the
 * same way vcsgui_open() does. Returns 1 on success. */
int vcsgui_init_repo(VcsGuiSession *s, const char *root);

/* Re-reads status/log/graph/HEAD/branches from disk into the session's
 * cached fields (freeing the previous contents first). Called
 * automatically by every mutating vcsgui_*() call below on success; a
 * frontend only needs to call this directly after noticing the working
 * tree changed outside its control (e.g. window regained focus). Returns
 * 1 on success. */
int vcsgui_refresh(VcsGuiSession *s);

/* Frees every cached list/graph and closes the repo handle. Safe to call
 * on an unopened or already-closed session. Does not touch anything on
 * disk. */
void vcsgui_close(VcsGuiSession *s);

/* author is copied into s->author and then wins over the configured
 * identity (vcsgui_identity()); NULL/"" uses the configuration. */
void vcsgui_set_author(VcsGuiSession *s, const char *author);

int vcsgui_add(VcsGuiSession *s, const char *relpath);
int vcsgui_remove(VcsGuiSession *s, const char *relpath);
int vcsgui_add_all(VcsGuiSession *s);
int vcsgui_commit(VcsGuiSession *s, const char *message, int stage_all, VcsOid *out_oid);

int vcsgui_branch_create(VcsGuiSession *s, const char *name);
int vcsgui_switch(VcsGuiSession *s, const char *branch);
int vcsgui_checkout(VcsGuiSession *s);
int vcsgui_merge(VcsGuiSession *s, const char *branch);

/* Clone/push/pull -- wrappers around remote.h that resolve the remote
 * and login from the repository's .amivcs/config (vcsconfig.h), arm this
 * session for vcsgui_install_progress_hook()'s bridge (see below) for the
 * call's duration, and refresh/open the session afterward on success.
 *
 * opts: program options only (verify_ssl, timeout -- the global config,
 * vcs_global_load_options()). url: NULL/"" = the repository's remote=;
 * given, it is used and, on success, remembered (a push also creates the
 * repository on the server). login: user/password typed in -- empty
 * fields fall back to what the repository remembers; remember=1 keeps
 * the password in .amivcs/config. If an operation fails and
 * vcsgui_auth_failed() is 1, ask for user/password and call again. */
typedef struct {
    char user[64];
    char pass[128];
    int  remember;
} VcsGuiLogin;

int vcsgui_clone(VcsGuiSession *s, const AcConfig *opts, const char *url,
                 const VcsGuiLogin *login, const char *local_dir);
int vcsgui_push(VcsGuiSession *s, const AcConfig *opts, const char *url, const VcsGuiLogin *login);
int vcsgui_pull(VcsGuiSession *s, const AcConfig *opts, const char *url, const VcsGuiLogin *login,
                const char *branch);
int vcsgui_auth_failed(void);

/* What the repository remembers about its remote: URL, user, whether a
 * password is stored. Returns 1 if a remote URL is set. */
int vcsgui_remote_info(VcsGuiSession *s, char *url, size_t usz, char *user, size_t nsz, int *has_pass);

/* The commit author (UTF-8): vcsgui_set_author()'s if set, else the
 * repository's name/email, else the global defaults (vcsconfig.h).
 * Returns 0 if no name is set anywhere -- ask the user and store it with
 * vcs_global_set("name"/"email") before committing. */
int vcsgui_identity(VcsGuiSession *s, char *out, size_t sz);

/* Requests that the currently-running clone/push/pull (if any) stop at
 * its next object boundary. Safe to call from another thread (Qt's
 * TransferJob pattern); a no-op if nothing is running. */
void vcsgui_request_cancel(VcsGuiSession *s);

/* Installs libcloudvcsgui's progress AND notify bridges as the process's
 * ui_hooks.h hooks -- call once at frontend startup, before any
 * vcsgui_*() call. Progress forwards to whichever VcsGuiSession is
 * currently running a remote operation (if any), honoring that
 * session's cancel_requested flag and progress_cb, exactly as before
 * (the function was originally progress-only; renamed to
 * vcsgui_install_hooks() now that it installs notify capture too --
 * see vcsgui_last_message()). confirm/overwrite are left at their
 * console defaults, unused by AMIVCS (no per-file overwrite prompts). */
void vcsgui_install_hooks(void);

/* The most recent message repo.c/merge.c/remote.c reported via
 * ui_notify() (see ui_hooks.h) since vcsgui_install_hooks() was called --
 * "" if none yet. A frontend reads this right after any vcsgui_*() call
 * returns 0 (or, for an informational one like "Fast-forward."/"Merge
 * completed.", after one returns 1) instead of hardcoding its own guess
 * at what went wrong: this is the exact, accurate reason the library
 * itself gave. Overwritten by the next ui_notify() call from ANY
 * vcsgui_*() operation -- read it immediately after the call whose
 * message you want, not later. Never NULL. */
const char *vcsgui_last_message(void);

#endif /* CLOUDVCSGUI_SESSION_H */
