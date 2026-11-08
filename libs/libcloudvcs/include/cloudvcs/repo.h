/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_REPO_H
#define CLOUDVCS_REPO_H

/*
 * repo.h -- repository/version-control logic (init/status/commit/log/
 * branch/switch/checkout, docs/adr/09_ADR_VCS_Design.md's v1 command
 * set). Orchestrates object.h + objstore.h + refs.h + the working tree;
 * this is the layer apps/amivcs's CLI (and, later, a GUI) calls
 * into -- neither frontend touches object/objstore/refs directly.
 *
 * v1 simplification (deliberate, not an oversight): AMIVCS must be run
 * from the repository root. There is no upward directory search like
 * Git's ".git above cwd" walk -- AmigaDOS paths don't have a portable
 * ".." (parent access uses a leading "/", a different syntax per volume
 * context), so climbing back up would need its own platform-specific
 * logic for zero v1 benefit. Running elsewhere just fails with a clear
 * error.
 *
 * File-level staging area (index.h) -- a deliberate reversal of ADR 09's
 * original "kein Staging/Index" decision, made for the AMIVCS-GUI plan's
 * git-gui-style staged/unstaged split. commit() now commits whatever is
 * staged in the index, not an unconditional whole-tree snapshot; pass
 * stage_all to get the old "stage everything, then commit" behaviour in
 * one call.
 */

#include "object.h"
#include "objstore.h"
#include "refs.h"
#include "mergestate.h"

#define VCS_IGNORE_FILE ".amivcsignore"

typedef struct {
    char root[1024];        /* working-tree root (repo top level) */
    char amivcs_dir[1024];  /* root/.amivcs */
    VcsObjStore store;
} VcsRepo;

/* Creates a new repository at root: <root>/.amivcs with an empty object
 * store, no branches yet, and HEAD attached to "main" (no commits until
 * the first vcs_repo_commit()). Also drops a default .amivcsignore next
 * to it if one doesn't already exist. Returns 1 on success, 0 if root is
 * already a repository or on an I/O error. */
int vcs_repo_init(const char *root);

/* Opens an existing repository rooted at root. Returns 1 on success, 0 if
 * root is not an AMIVCS repository (no <root>/.amivcs). */
int vcs_repo_open(VcsRepo *repo, const char *root);

/* One New/Modified/Deleted difference between two tree states. */
typedef struct {
    char kind;       /* 'N' new, 'M' modified, 'D' deleted */
    char path[1024];
} VcsChange;

typedef struct {
    VcsChange *items;
    int n, cap;
} VcsChangeList;

/* Split status (index.h's staging area): staged = index vs HEAD (what
 * commit() will record), unstaged = working tree vs index (what add()
 * would pick up). A repo whose index still implicitly mirrors HEAD (see
 * vcs_index_load()) reports an empty `staged` -- exactly like a fresh
 * git repo before the first `git add`. */
/* merge: the merge in progress (mergestate.h; merge.active 0 if none) --
 * its conflict entries (vcs_mergestate_is_conflict()) are what status
 * shows as "Unmerged paths". The <path>.theirs files that merge wrote
 * are left out of `unstaged`, so `add -A` doesn't pick them up. */
typedef struct {
    VcsChangeList staged;
    VcsChangeList unstaged;
    VcsMergeState merge;
} VcsStatus;

/* Fills *out with the current staged/unstaged change lists. Returns 1 on
 * success, 0 on I/O error. Caller must vcs_status_free(out) when done. */
int  vcs_repo_status_raw(VcsRepo *repo, VcsStatus *out);
void vcs_status_free(VcsStatus *out);

/* Prints "Changes to be committed:" / "Changes not staged for commit:"
 * sections (vcs_repo_status_raw()'s console rendering) to stdout. Returns
 * 1 on success (even with zero changes), 0 on I/O error. */
int vcs_repo_status(VcsRepo *repo);

/* 1 if the working tree differs from HEAD's committed tree, 0 if clean,
 * -1 on I/O error. Used by switch()/checkout() to refuse a materialize
 * that would silently discard uncommitted work (plan section 12: v1
 * behaviour on a dirty tree is to abort, not overwrite). */
int vcs_repo_is_dirty(VcsRepo *repo);

/* Recursively snapshots repo->root (skipping VCS_DIR_NAME and
 * .amivcsignore matches) into blob/tree objects in repo->store, without
 * touching any ref. *out_tree_oid receives the resulting root tree's
 * OID. Shared by vcs_repo_commit() and, later, merge.c. Returns 1 on
 * success. */
int vcs_repo_snapshot_workdir(VcsRepo *repo, VcsOid *out_tree_oid);

/* Names: every path this API takes or returns is a repository path
 * (UTF-8, vcsname.h); author and message too. vcs_repo_learn_names()
 * walks HEAD's and the index's trees once, so vcsname.h can map a local
 * name the codeset can't show exactly back to its repository name --
 * call it before vcs_path_from_local() on a path the user typed. */
void vcs_repo_learn_names(VcsRepo *repo);

/* A branch name the user typed (local codeset) as the repository knows
 * it: UTF-8, or the raw bytes if an older Amiga build created the branch
 * that way. malloc'd; NULL only when out of memory. */
char *vcs_repo_branch_name(VcsRepo *repo, const char *local_name);

/* Write lock: .amivcs/lock while a command changes the repository or the
 * working tree, so the CLI, amivcs-gui and the IDE never write at the
 * same time. Acquire returns 0 (after a UI_ERROR) if the file exists --
 * another program is at work, or one was killed: then the message says
 * to remove the file. Release removes it. */
int  vcs_repo_lock(const char *amivcs_dir);
void vcs_repo_unlock(const char *amivcs_dir);

/* Materializes the given tree object into repo->root: writes/overwrites
 * files and directories to match the tree, and removes anything present
 * on disk but absent from the tree and not covered by .amivcsignore
 * (ignored paths are left alone, e.g. build output). Shared by
 * checkout()/switch() and, later, merge.c. Returns 1 on success. */
int vcs_repo_materialize_tree(VcsRepo *repo, const VcsOid *tree_oid);

/* Writes a commit object from the current index (parent = HEAD's current
 * commit, or no parent for the first commit ever) and advances the
 * current branch to it. Refuses (0) if HEAD is detached -- switch to a
 * branch first -- or if the index is identical to HEAD's tree (nothing
 * staged to commit). author may be NULL/empty (defaults to "unknown").
 * stage_all stages every unstaged change first (index.h's
 * vcs_index_add_all()) -- the CLI's `-a`/`--all`, git convention: without
 * it, a plain commit with nothing previously add()ed commits nothing.
 * out_oid, if non-NULL, receives the new commit's OID.
 * During a merge (mergestate.h) it refuses while a conflict is
 * unresolved; otherwise the commit gets both parents (HEAD and the
 * merged commit), an empty message becomes "Merge branch '<label>'",
 * and the merge state and the merge's .theirs files are removed. */
int vcs_repo_commit(VcsRepo *repo, const char *author, const char *message, int stage_all, VcsOid *out_oid);

/* One commit's history-view data: full parent list (not just first-
 * parent), so a real DAG/graph can be built from it (libcloudvcsgui's
 * graph layout). decoration is a ", "-joined list of branch short names
 * whose ref currently points exactly at oid, "" if none. */
typedef struct {
    VcsOid oid;
    VcsOid parents[VCS_MAX_PARENTS];
    int    nparents;
    long   mtime;
    char   author[64];
    char  *message;       /* malloc'd, NUL-terminated; owned by the entry */
    char   decoration[256];
} VcsLogEntry;

typedef struct {
    VcsLogEntry *items;
    int n, cap;
} VcsLogList;

void vcs_loglist_free(VcsLogList *list);

/* Walks HEAD's parent chain only (newest first) -- the CLI's plain `log`
 * stays first-parent/console-narrow on purpose, a merged-in branch's own
 * commits aren't repeated here. Returns 1 on success. */
int vcs_repo_log_raw(VcsRepo *repo, VcsLogList *out);

/* Every commit reachable from ANY branch tip (refs.h's packed-refs), each
 * exactly once (OID-set-deduplicated, same idea as merge.c's ancestor
 * walk), newest-mtime-first ("git log --all --date-order") -- the data
 * source for the GUI's full multi-branch history graph
 * (libcloudvcsgui's vcsgui_compute_graph_layout()). Returns 1 on
 * success. */
int vcs_repo_log_all_raw(VcsRepo *repo, VcsLogList *out);

/* Walks HEAD's parent chain (newest first) printing each commit's OID,
 * date, author and message to stdout (vcs_repo_log_raw()'s console
 * rendering). Returns 1 on success. */
int vcs_repo_log(VcsRepo *repo);

/* name == NULL: lists all branches to stdout, marking the current one.
 * name set: creates a new branch pointing at HEAD's current commit.
 * Returns 1 on success, 0 if name already exists or HEAD has no commits
 * yet to branch from. */
int vcs_repo_branch(VcsRepo *repo, const char *name);

/* Points HEAD at branch and materializes its commit into the working
 * tree. Refuses (0) if the working tree is dirty (see vcs_repo_is_dirty())
 * or if branch doesn't exist. */
int vcs_repo_switch(VcsRepo *repo, const char *branch);

/* Re-materializes HEAD's current commit into the working tree. Refuses
 * (0) if the working tree is dirty, exactly like switch() -- v1
 * deliberately never overwrites uncommitted work silently (plan section
 * 12); commit first, or remove the offending files by hand, before
 * retrying. */
int vcs_repo_checkout(VcsRepo *repo);

#endif /* CLOUDVCS_REPO_H */
