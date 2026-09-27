/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * merge.c -- see merge.h.
 */
#include "merge.h"
#include "index.h"
#include "treeutil.h"
#include "oidset.h"
#include "localfs.h"
#include "ui_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* VcsOidSet (dynamic array + linear membership check) is shared with
 * remote.c's object-graph walks -- see oidset.h. */

static void commit_parents(const VcsObjStore *store, const VcsOid *oid, VcsOid parents[VCS_MAX_PARENTS], int *nparents)
{
    VcsObjType type;
    void *data;
    long len;
    VcsCommit c;
    int i;

    *nparents = 0;
    if (vcs_oid_is_zero(oid)) return;
    if (!vcsobjstore_read(store, oid, &type, &data, &len)) return;
    vcs_commit_init(&c);
    if (type == VCS_OBJ_COMMIT && vcs_commit_decode(data, len, &c)) {
        *nparents = c.nparents;
        for (i = 0; i < c.nparents; i++) parents[i] = c.parents[i];
    }
    free(data);
    vcs_commit_free(&c);
}

/* All ancestors of start, start itself included (unless start is zero --
 * an empty branch has no ancestors at all). */
static void collect_ancestors(const VcsObjStore *store, const VcsOid *start, VcsOidSet *out)
{
    VcsOidSet stack;
    vcs_oidset_init(&stack);
    if (!vcs_oid_is_zero(start)) vcs_oidset_add(&stack, start);

    while (stack.n > 0) {
        VcsOid cur = stack.items[--stack.n];
        VcsOid parents[VCS_MAX_PARENTS];
        int nparents, i;

        if (vcs_oidset_contains(out, &cur)) continue;
        vcs_oidset_add(out, &cur);

        commit_parents(store, &cur, parents, &nparents);
        for (i = 0; i < nparents; i++) vcs_oidset_add(&stack, &parents[i]);
    }
    vcs_oidset_free(&stack);
}

/* Finds *a* common ancestor of ours/theirs (not necessarily the unique
 * lowest one in a history with multiple merge points -- v1 doesn't need
 * that precision, ADR 09's "simple 3-way merge", not full Git parity):
 * BFS out from theirs, return the first commit already known to be an
 * ancestor of ours. Returns 1 with *out_base set, or 0 if the two
 * histories share no ancestor at all (unrelated repositories). */
static int find_merge_base(const VcsObjStore *store, const VcsOid *ours, const VcsOid *theirs, VcsOid *out_base)
{
    VcsOidSet ours_ancestors, queue, visited;
    int qi = 0, found = 0;

    vcs_oidset_init(&ours_ancestors);
    collect_ancestors(store, ours, &ours_ancestors);

    vcs_oidset_init(&queue);
    vcs_oidset_init(&visited);
    if (!vcs_oid_is_zero(theirs)) vcs_oidset_add(&queue, theirs);

    while (qi < queue.n) {
        VcsOid cur = queue.items[qi++];
        VcsOid parents[VCS_MAX_PARENTS];
        int nparents, i;

        if (vcs_oidset_contains(&visited, &cur)) continue;
        vcs_oidset_add(&visited, &cur);

        if (vcs_oidset_contains(&ours_ancestors, &cur)) { *out_base = cur; found = 1; break; }

        commit_parents(store, &cur, parents, &nparents);
        for (i = 0; i < nparents; i++) vcs_oidset_add(&queue, &parents[i]);
    }

    vcs_oidset_free(&queue);
    vcs_oidset_free(&visited);
    vcs_oidset_free(&ours_ancestors);
    if (!found) *out_base = VCS_OID_ZERO;
    return found;
}

/* ------------------------------------------------------------------ */

static void ensure_parent_dir(const char *root, const char *relpath)
{
    const char *slash = strrchr(relpath, '/');
    if (!slash) return;
    {
        char reldir[1024], absdir[1024];
        size_t len = (size_t)(slash - relpath);
        if (len >= sizeof(reldir)) len = sizeof(reldir) - 1;
        memcpy(reldir, relpath, len);
        reldir[len] = '\0';
        localfs_join(absdir, sizeof(absdir), root, reldir);
        localfs_mkdir_p(absdir);
    }
}

int vcs_repo_merge(VcsRepo *repo, const char *branch)
{
    VcsRefList refs;
    VcsRef *theirs_ref;
    char fullname[128];
    VcsOid theirs_commit;

    if (!vcs_refs_load(repo->amivcs_dir, &refs)) return 0;
    snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, branch);
    theirs_ref = vcs_refs_find(&refs, fullname);
    if (!theirs_ref) {
        ui_notify(UI_ERROR, "amivcs: no such branch '%s'", branch);
        vcs_refs_free(&refs);
        return 0;
    }
    theirs_commit = theirs_ref->oid;
    vcs_refs_free(&refs);

    return vcs_repo_merge_commit(repo, &theirs_commit, branch);
}

int vcs_repo_merge_commit(VcsRepo *repo, const VcsOid *theirs_commit_in, const char *theirs_label)
{
    VcsRefList refs;
    char fullname[128];
    VcsHead head;
    VcsOid ours_commit, theirs_commit, base_commit;
    int dirty;

    theirs_commit = *theirs_commit_in;

    dirty = vcs_repo_is_dirty(repo);
    if (dirty != 0) {
        ui_notify(UI_ERROR, dirty < 0
            ? "amivcs: could not check working tree state"
            : "amivcs: working tree has uncommitted changes -- commit first");
        return 0;
    }

    if (!vcs_head_load(repo->amivcs_dir, &head)) return 0;
    if (head.detached) { ui_notify(UI_ERROR, "amivcs: HEAD is detached -- switch to a branch first"); return 0; }

    if (!vcs_refs_load(repo->amivcs_dir, &refs)) return 0;

    snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, head.branch);
    {
        VcsRef *ours_ref = vcs_refs_find(&refs, fullname);
        ours_commit = ours_ref ? ours_ref->oid : VCS_OID_ZERO;
    }

    if (vcs_oid_equal(&ours_commit, &theirs_commit) || vcs_oid_is_zero(&theirs_commit)) {
        ui_notify(UI_INFO, "Already up to date.");
        vcs_refs_free(&refs);
        return 1;
    }

    if (vcs_oid_is_zero(&ours_commit)) {
        /* Current branch has no commits yet: trivially fast-forward to theirs. */
        base_commit = ours_commit;
    } else if (!find_merge_base(&repo->store, &ours_commit, &theirs_commit, &base_commit)) {
        ui_notify(UI_ERROR, "amivcs: '%s' and the current branch share no common history", theirs_label);
        vcs_refs_free(&refs);
        return 0;
    }

    if (vcs_oid_equal(&base_commit, &theirs_commit)) {
        ui_notify(UI_INFO, "Already up to date.");
        vcs_refs_free(&refs);
        return 1;
    }

    if (vcs_oid_equal(&base_commit, &ours_commit)) {
        /* Fast-forward: ours hasn't diverged, just move the branch. */
        VcsOid theirs_tree;
        if (!vcs_commit_tree(&repo->store, &theirs_commit, &theirs_tree)) { vcs_refs_free(&refs); return 0; }
        if (!vcs_repo_materialize_tree(repo, &theirs_tree)) { vcs_refs_free(&refs); return 0; }
        vcs_index_reset_to(repo->amivcs_dir, &theirs_tree);

        snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, head.branch);
        vcs_refs_upsert(&refs, fullname, &theirs_commit);
        if (!vcs_refs_save(repo->amivcs_dir, &refs)) { vcs_refs_free(&refs); return 0; }
        vcs_refs_free(&refs);
        ui_notify(UI_INFO, "Fast-forward.");
        return 1;
    }
    vcs_refs_free(&refs);

    /* Real 3-way merge: flatten base/ours/theirs trees to (path -> oid)
     * and walk all three in lock-step, deciding per path (see merge.h). */
    {
        VcsOid base_tree, ours_tree, theirs_tree;
        VcsFlatList base_flat, ours_flat, theirs_flat;
        int bi = 0, oi = 0, ti = 0;
        int conflicts = 0;

        if (!vcs_commit_tree(&repo->store, &base_commit, &base_tree)) return 0;
        if (!vcs_commit_tree(&repo->store, &ours_commit, &ours_tree)) return 0;
        if (!vcs_commit_tree(&repo->store, &theirs_commit, &theirs_tree)) return 0;

        vcs_flat_init(&base_flat);
        vcs_flat_init(&ours_flat);
        vcs_flat_init(&theirs_flat);
        if (!vcs_flatten_committed_tree(&repo->store, &base_tree, "", &base_flat)
            || !vcs_flatten_committed_tree(&repo->store, &ours_tree, "", &ours_flat)
            || !vcs_flatten_committed_tree(&repo->store, &theirs_tree, "", &theirs_flat)) {
            vcs_flat_free(&base_flat); vcs_flat_free(&ours_flat); vcs_flat_free(&theirs_flat);
            return 0;
        }
        vcs_flat_sort(&base_flat);
        vcs_flat_sort(&ours_flat);
        vcs_flat_sort(&theirs_flat);

        while (bi < base_flat.n || oi < ours_flat.n || ti < theirs_flat.n) {
            const char *path = NULL;
            const VcsFlatEntry *b = bi < base_flat.n ? &base_flat.items[bi] : NULL;
            const VcsFlatEntry *o = oi < ours_flat.n ? &ours_flat.items[oi] : NULL;
            const VcsFlatEntry *t = ti < theirs_flat.n ? &theirs_flat.items[ti] : NULL;

            /* Lowest path among the three current cursors -- same idea as
             * libcloudsync's pick_min_path() k-way merge-scan. */
            if (b && (!path || strcmp(b->path, path) < 0)) path = b->path;
            if (o && (!path || strcmp(o->path, path) < 0)) path = o->path;
            if (t && (!path || strcmp(t->path, path) < 0)) path = t->path;

            {
                int in_base = b && strcmp(b->path, path) == 0;
                int in_ours = o && strcmp(o->path, path) == 0;
                int in_theirs = t && strcmp(t->path, path) == 0;
                const VcsOid *base_oid = in_base ? &b->oid : NULL;
                const VcsOid *ours_oid = in_ours ? &o->oid : NULL;
                const VcsOid *theirs_oid = in_theirs ? &t->oid : NULL;
                enum { KEEP_OURS, TAKE_THEIRS, DELETE, CONFLICT } action;

                if (in_ours && in_theirs && vcs_oid_equal(ours_oid, theirs_oid)) {
                    action = KEEP_OURS; /* identical on both sides */
                } else if (in_base && in_ours && in_theirs) {
                    int ours_changed = !vcs_oid_equal(ours_oid, base_oid);
                    int theirs_changed = !vcs_oid_equal(theirs_oid, base_oid);
                    action = !ours_changed ? TAKE_THEIRS : !theirs_changed ? KEEP_OURS : CONFLICT;
                } else if (!in_base && in_ours && in_theirs) {
                    action = CONFLICT; /* added differently on both sides */
                } else if (in_base && in_ours && !in_theirs) {
                    action = vcs_oid_equal(ours_oid, base_oid) ? DELETE : CONFLICT;
                } else if (in_base && !in_ours && in_theirs) {
                    action = vcs_oid_equal(theirs_oid, base_oid) ? DELETE : CONFLICT;
                } else if (in_base && !in_ours && !in_theirs) {
                    action = DELETE; /* deleted on both sides independently */
                } else if (!in_base && in_ours && !in_theirs) {
                    action = KEEP_OURS; /* only we ever had it */
                } else { /* !in_base && !in_ours && in_theirs */
                    action = TAKE_THEIRS; /* only they have it */
                }

                switch (action) {
                case KEEP_OURS:
                    break; /* working tree already matches ours */
                case DELETE: {
                    char abs_path[1024];
                    localfs_join(abs_path, sizeof(abs_path), repo->root, path);
                    localfs_remove(abs_path);
                    break;
                }
                case TAKE_THEIRS: {
                    char abs_path[1024];
                    ensure_parent_dir(repo->root, path);
                    localfs_join(abs_path, sizeof(abs_path), repo->root, path);
                    vcsobjstore_extract_blob_to_file(&repo->store, theirs_oid, abs_path);
                    break;
                }
                case CONFLICT: {
                    conflicts++;
                    ui_notify(UI_WARN, "conflict: %s", path);
                    if (in_theirs) {
                        char abs_path[1024], theirs_path[1040];
                        ensure_parent_dir(repo->root, path);
                        localfs_join(abs_path, sizeof(abs_path), repo->root, path);
                        snprintf(theirs_path, sizeof(theirs_path), "%s.theirs", abs_path);
                        vcsobjstore_extract_blob_to_file(&repo->store, theirs_oid, theirs_path);
                    }
                    break;
                }
                }
            }

            if (b && strcmp(b->path, path) == 0) bi++;
            if (o && strcmp(o->path, path) == 0) oi++;
            if (t && strcmp(t->path, path) == 0) ti++;
        }

        vcs_flat_free(&base_flat);
        vcs_flat_free(&ours_flat);
        vcs_flat_free(&theirs_flat);

        if (conflicts > 0) {
            ui_notify(UI_ERROR, "amivcs: %d conflict(s) -- resolve by hand (see the .theirs files), "
                            "then commit; the merge was NOT completed", conflicts);
            return 0;
        }
    }

    {
        VcsOid merged_tree, merge_commit_oid;
        VcsCommit commit;
        void *data;
        long len;

        if (!vcs_repo_snapshot_workdir(repo, &merged_tree)) return 0;

        vcs_commit_init(&commit);
        commit.tree = merged_tree;
        commit.parents[0] = ours_commit;
        commit.parents[1] = theirs_commit;
        commit.nparents = 2;
        commit.mtime = (long)time(NULL);
        strncpy(commit.author, "amivcs", sizeof(commit.author) - 1);
        {
            char msg[192];
            snprintf(msg, sizeof(msg), "Merge branch '%s'", theirs_label);
            commit.message = (char *)malloc(strlen(msg) + 1);
            if (commit.message) strcpy(commit.message, msg);
        }

        if (!vcs_commit_encode(&commit, &data, &len)) { vcs_commit_free(&commit); return 0; }
        vcs_commit_free(&commit);

        if (!vcsobjstore_write(&repo->store, VCS_OBJ_COMMIT, data, len, &merge_commit_oid)) { free(data); return 0; }
        free(data);

        if (!vcs_refs_load(repo->amivcs_dir, &refs)) return 0;
        snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, head.branch);
        vcs_refs_upsert(&refs, fullname, &merge_commit_oid);
        if (!vcs_refs_save(repo->amivcs_dir, &refs)) { vcs_refs_free(&refs); return 0; }
        vcs_refs_free(&refs);
        vcs_index_reset_to(repo->amivcs_dir, &merged_tree);

        ui_notify(UI_INFO, "Merge completed.");
    }
    return 1;
}
