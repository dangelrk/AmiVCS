/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * merge.c -- see merge.h.
 */
#include "merge.h"
#include "merge3.h"
#include "mergestate.h"
#include "diff.h"
#include "index.h"
#include "treeutil.h"
#include "oidset.h"
#include "localfs.h"
#include "vcsname.h"
#include "vcsconfig.h"
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
        vcs_local_join(absdir, sizeof(absdir), root, reldir);
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
        ui_notify(UI_ERROR, "amivcs: no such branch '%s'", vcs_shown(branch));
        vcs_refs_free(&refs);
        return 0;
    }
    theirs_commit = theirs_ref->oid;
    vcs_refs_free(&refs);

    return vcs_repo_merge_commit(repo, &theirs_commit, branch);
}

/* after deleting a file: its parent directories while they are empty */
static void prune_parents(const char *root, const char *repo_path)
{
    char local[1024], abs_dir[1024];
    char *slash;
    if (vcs_path_to_local(repo_path, local, sizeof(local)) < 0) return;
    while ((slash = strrchr(local, '/')) != NULL) {
        *slash = '\0';
        localfs_join(abs_dir, sizeof(abs_dir), root, local);
        if (!localfs_remove(abs_dir)) break;
    }
}

/* What the merge does with one path, decided for all paths before the
 * first file is written (so the merge state can be saved first). */
enum {
    STEP_DELETE,        /* remove from working tree and index */
    STEP_WRITE,         /* oid into working tree and index (theirs, or a clean line merge) */
    STEP_MARKERS,       /* conflict: oid (text with markers) into the working tree only */
    STEP_THEIRS_FILE,   /* conflict: theirs' oid as <path>.theirs, path keeps ours */
    STEP_KEEP_CONFLICT, /* conflict: ours stays as it is (modified here, deleted there) */
    STEP_WORK_ONLY      /* conflict: theirs' oid into the working tree only (deleted here) */
};

typedef struct {
    const char *path;   /* points into one of the flat lists */
    int         step;
    VcsOid      oid;
    const char *kind;   /* mergestate kind */
} MergeStep;

typedef struct {
    MergeStep *items;
    int n, cap;
} MergeSteps;

static int steps_add(MergeSteps *s, const char *path, int step, const VcsOid *oid, const char *kind)
{
    if (s->n == s->cap) {
        int newcap = s->cap ? s->cap * 2 : 32;
        MergeStep *tmp = (MergeStep *)realloc(s->items, sizeof(MergeStep) * (size_t)newcap);
        if (!tmp) return 0;
        s->items = tmp;
        s->cap = newcap;
    }
    s->items[s->n].path = path;
    s->items[s->n].step = step;
    s->items[s->n].oid = oid ? *oid : VCS_OID_ZERO;
    s->items[s->n].kind = kind;
    s->n++;
    return 1;
}

/* Line merge of one text file both sides changed (base_oid NULL: added
 * on both sides). Returns 1 with *out_oid the merged blob and
 * *conflicts set, 0 if a side is binary or too big (or on I/O error):
 * then the file is merged as a whole. */
static int try_line_merge(VcsRepo *repo, const VcsOid *base_oid, const VcsOid *ours_oid,
                          const VcsOid *theirs_oid, const char *ours_label, const char *theirs_label,
                          VcsOid *out_oid, int *conflicts)
{
    void *b = NULL, *o = NULL, *t = NULL;
    long bl = 0, ol = 0, tl = 0;
    int ok = 0;

    if (base_oid && vcsobjstore_read_blob_max(&repo->store, base_oid, VCS_DIFF_MAX_BYTES, &b, &bl) != 1) goto done;
    if (vcsobjstore_read_blob_max(&repo->store, ours_oid, VCS_DIFF_MAX_BYTES, &o, &ol) != 1) goto done;
    if (vcsobjstore_read_blob_max(&repo->store, theirs_oid, VCS_DIFF_MAX_BYTES, &t, &tl) != 1) goto done;
    if ((b && !vcs_diff_is_text(b, bl)) || !vcs_diff_is_text(o, ol) || !vcs_diff_is_text(t, tl)) goto done;
    {
        char *merged;
        long mlen;
        if (!vcs_merge3(b ? (const char *)b : "", bl, (const char *)o, ol, (const char *)t, tl,
                        ours_label, theirs_label, &merged, &mlen, conflicts))
            goto done;
        ok = vcsobjstore_write(&repo->store, VCS_OBJ_BLOB, merged, mlen, out_oid);
        free(merged);
    }
done:
    free(b);
    free(o);
    free(t);
    return ok;
}

int vcs_repo_merge_commit(VcsRepo *repo, const VcsOid *theirs_commit_in, const char *theirs_label)
{
    VcsRefList refs;
    char fullname[128];
    VcsHead head;
    VcsOid ours_commit, theirs_commit, base_commit;
    int dirty;

    theirs_commit = *theirs_commit_in;

    if (vcs_mergestate_active(repo->amivcs_dir)) {
        ui_notify(UI_ERROR, "amivcs: a merge is in progress -- resolve the conflicts and commit, "
                            "or run 'merge --abort'");
        return 0;
    }

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
        ui_notify(UI_ERROR, "amivcs: '%s' and the current branch share no common history", vcs_shown(theirs_label));
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

    /* Real 3-way merge: flatten base/ours/theirs trees to (path -> oid),
     * decide every path (pass 1), save the merge state, then write
     * (pass 2). */
    {
        VcsOid base_tree, ours_tree, theirs_tree;
        VcsFlatList base_flat, ours_flat, theirs_flat;
        MergeSteps steps;
        VcsMergeState st;
        int bi = 0, oi = 0, ti = 0, i, ok = 1;
        int conflicts = 0;
        char ours_label[200], theirs_lbl[200];

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

        /* names of both sides (vcsname.h); a theirs name that would be
         * the same file as a different ours name here stops the merge
         * before anything is written */
        vcs_names_begin_flat(repo, &ours_flat, &theirs_flat);
        if (!vcs_names_unique_flat(&ours_flat, &theirs_flat)
            || !vcs_names_in_the_way(repo, &ours_flat, &theirs_flat)) {
            vcs_flat_free(&base_flat); vcs_flat_free(&ours_flat); vcs_flat_free(&theirs_flat);
            return 0;
        }

        /* marker labels in the codeset the files are written in */
        {
            char *l = vcs_text_to_local(head.branch);
            snprintf(ours_label, sizeof(ours_label), "ours (%s)", l ? l : head.branch);
            free(l);
            l = vcs_text_to_local(theirs_label);
            snprintf(theirs_lbl, sizeof(theirs_lbl), "theirs (%s)", l ? l : theirs_label);
            free(l);
        }

        steps.items = NULL;
        steps.n = steps.cap = 0;

        /* ---- pass 1: decide ---- */
        while (ok && (bi < base_flat.n || oi < ours_flat.n || ti < theirs_flat.n)) {
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

                if (in_ours && in_theirs && vcs_oid_equal(ours_oid, theirs_oid)) {
                    /* identical on both sides */
                } else if (in_ours && in_theirs) {
                    int ours_changed = !in_base || !vcs_oid_equal(ours_oid, base_oid);
                    int theirs_changed = !in_base || !vcs_oid_equal(theirs_oid, base_oid);
                    if (!ours_changed) {
                        ok = steps_add(&steps, path, STEP_WRITE, theirs_oid, "touched");
                    } else if (theirs_changed) {
                        /* changed (or added) differently on both sides */
                        const char *kind = in_base ? "content" : "add-add";
                        VcsOid merged;
                        int nc = 0;
                        if (try_line_merge(repo, base_oid, ours_oid, theirs_oid, ours_label, theirs_lbl,
                                           &merged, &nc)) {
                            if (nc == 0) {
                                ok = steps_add(&steps, path, STEP_WRITE, &merged, "touched");
                            } else {
                                ok = steps_add(&steps, path, STEP_MARKERS, &merged, kind);
                                conflicts++;
                            }
                        } else {
                            ok = steps_add(&steps, path, STEP_THEIRS_FILE, theirs_oid,
                                           in_base ? "binary" : "add-add");
                            conflicts++;
                        }
                    }
                } else if (in_base && in_ours && !in_theirs) {
                    if (vcs_oid_equal(ours_oid, base_oid)) {
                        ok = steps_add(&steps, path, STEP_DELETE, NULL, "touched");
                    } else {
                        ok = steps_add(&steps, path, STEP_KEEP_CONFLICT, NULL, "modify-delete");
                        conflicts++;
                    }
                } else if (in_base && !in_ours && in_theirs) {
                    if (!vcs_oid_equal(theirs_oid, base_oid)) {
                        ok = steps_add(&steps, path, STEP_WORK_ONLY, theirs_oid, "delete-modify");
                        conflicts++;
                    } /* else: deleted here, unchanged there -- stays deleted */
                } else if (!in_base && !in_ours && in_theirs) {
                    ok = steps_add(&steps, path, STEP_WRITE, theirs_oid, "touched"); /* only they have it */
                }
                /* deleted on both sides, or only we have it: nothing to do */
            }

            if (b && strcmp(b->path, path) == 0) bi++;
            if (o && strcmp(o->path, path) == 0) oi++;
            if (t && strcmp(t->path, path) == 0) ti++;
        }

        /* ---- a <path>.theirs copy must not replace anything: not a file
         * of the user, not a tracked file ---- */
        for (i = 0; ok && i < steps.n; i++) {
            const MergeStep *m = &steps.items[i];
            char rel[1040], abs_path[1040];
            if (m->step != STEP_THEIRS_FILE) continue;
            snprintf(rel, sizeof(rel), "%s.theirs", m->path);
            if (vcs_flat_find(&ours_flat, rel) || vcs_flat_find(&theirs_flat, rel)
                || (vcs_local_join(abs_path, sizeof(abs_path), repo->root, rel) && localfs_exists(abs_path))) {
                char *shown = vcs_text_to_local(rel);
                ui_notify(UI_ERROR, "amivcs: %s already exists, and the merge needs that name for the "
                          "other side of a conflict -- move or rename it, then merge again; "
                          "nothing was changed", shown ? shown : rel);
                free(shown);
                free(steps.items);
                vcs_flat_free(&base_flat); vcs_flat_free(&ours_flat); vcs_flat_free(&theirs_flat);
                return 0;
            }
        }

        /* ---- the merge state, saved before the first file is written:
         * an interrupted merge can still be aborted ---- */
        vcs_mergestate_init(&st);
        st.active = 1;
        st.theirs = theirs_commit;
        st.ours = ours_commit;
        vcs_utf8_copy(st.label, sizeof(st.label), theirs_label);
        for (i = 0; ok && i < steps.n; i++) {
            const MergeStep *m = &steps.items[i];
            int conflict = m->step == STEP_MARKERS || m->step == STEP_THEIRS_FILE
                        || m->step == STEP_KEEP_CONFLICT || m->step == STEP_WORK_ONLY;
            if (!vcs_mergestate_put(&st, conflict ? 'U' : '-', m->kind, m->step == STEP_THEIRS_FILE, m->path))
                ok = 0;
        }
        if (ok) ok = vcs_mergestate_save(repo->amivcs_dir, &st);
        vcs_mergestate_free(&st);
        /* theirs' names written below must still be known when the merge
         * (or, after conflicts, the user's commit) scans the tree */
        if (ok) vcs_names_save(repo->amivcs_dir);

        /* ---- pass 2: write ---- */
        for (i = 0; ok && i < steps.n; i++) {
            const MergeStep *m = &steps.items[i];
            char abs_path[1024];
            vcs_local_join(abs_path, sizeof(abs_path), repo->root, m->path);
            switch (m->step) {
            case STEP_DELETE:
                localfs_remove(abs_path);
                prune_parents(repo->root, m->path);
                if (!vcs_index_remove(repo, m->path)) ok = 0;   /* the merge result is built in the index */
                break;
            case STEP_WRITE:
                ensure_parent_dir(repo->root, m->path);
                if (!vcsobjstore_extract_blob_to_file(&repo->store, &m->oid, abs_path)
                    || !vcs_index_set_blob(repo, m->path, &m->oid))
                    ok = 0;
                break;
            case STEP_MARKERS:
            case STEP_WORK_ONLY:
                ensure_parent_dir(repo->root, m->path);
                if (!vcsobjstore_extract_blob_to_file(&repo->store, &m->oid, abs_path)) ok = 0;
                break;
            case STEP_THEIRS_FILE: {
                char theirs_path[1040];
                snprintf(theirs_path, sizeof(theirs_path), "%s.theirs", abs_path);
                ensure_parent_dir(repo->root, m->path);
                if (!vcsobjstore_extract_blob_to_file(&repo->store, &m->oid, theirs_path)) ok = 0;
                break;
            }
            case STEP_KEEP_CONFLICT:
                break;
            }
            if (m->step >= STEP_MARKERS) {
                char *shown = vcs_text_to_local(m->path);
                ui_notify(UI_WARN, "conflict (%s): %s", m->kind, shown ? shown : m->path);
                free(shown);
            }
        }

        free(steps.items);
        vcs_flat_free(&base_flat);
        vcs_flat_free(&ours_flat);
        vcs_flat_free(&theirs_flat);

        if (!ok) {
            ui_notify(UI_ERROR, "amivcs: the merge could not write all files -- "
                                "run 'merge --abort' to return to where you were");
            return 0;
        }
        if (conflicts > 0) {
            ui_notify(UI_ERROR, "amivcs: %d conflict(s) -- resolve them (edit the files, or "
                            "'resolve --ours/--theirs'), 'add' them, then commit; "
                            "'merge --abort' goes back", conflicts);
            return 0;
        }
    }

    {
        VcsOid merge_commit_oid;
        char ident[300];
        char *author = vcs_identity(repo->amivcs_dir, ident, sizeof(ident))
                           ? vcs_text_from_local(ident) : NULL;
        char msg[192];
        int ok;

        snprintf(msg, sizeof(msg), "Merge branch '%s'", theirs_label);
        /* vcs_repo_commit() sees the merge state and records both parents */
        ok = vcs_repo_commit(repo, author ? author : "amivcs", msg, 0, &merge_commit_oid);
        free(author);
        if (!ok) return 0;
        ui_notify(UI_INFO, "Merge completed.");
    }
    return 1;
}

/* ------------------------------------------------------------------ */

int vcs_repo_merge_abort(VcsRepo *repo)
{
    VcsMergeState st;
    VcsOid ours_tree;
    VcsFlatList ours_flat;
    int i, ok = 1;

    if (!vcs_mergestate_load(repo->amivcs_dir, &st)) {
        ui_notify(UI_ERROR, "amivcs: the merge state (.amivcs/MERGE_HEAD) is damaged -- remove "
                            ".amivcs/MERGE_HEAD and .amivcs/MERGE_FILES, then 'checkout' after "
                            "committing or moving your own changes");
        return 0;
    }
    if (!st.active) {
        ui_notify(UI_ERROR, "amivcs: no merge in progress");
        return 0;
    }
    if (!vcs_commit_tree(&repo->store, &st.ours, &ours_tree)) { vcs_mergestate_free(&st); return 0; }
    vcs_flat_init(&ours_flat);
    if (!vcs_flatten_committed_tree(&repo->store, &ours_tree, "", &ours_flat)) {
        vcs_flat_free(&ours_flat);
        vcs_mergestate_free(&st);
        return 0;
    }
    vcs_flat_sort(&ours_flat);
    vcs_names_begin_flat(repo, &ours_flat, NULL);   /* + the names the merge saved */

    /* exactly the paths the merge wrote or deleted -- a file the user
     * owns (not under version control) is never touched */
    for (i = 0; i < st.n; i++) {
        const VcsMergeFile *f = &st.items[i];
        const VcsFlatEntry *e = vcs_flat_find(&ours_flat, f->path);
        char abs_path[1024];
        if (!vcs_local_join(abs_path, sizeof(abs_path), repo->root, f->path)) continue;
        if (e) {
            ensure_parent_dir(repo->root, f->path);
            if (!vcsobjstore_extract_blob_to_file(&repo->store, &e->oid, abs_path)) ok = 0;
        } else if (localfs_exists(abs_path) && !localfs_is_dir(abs_path)) {
            localfs_remove(abs_path);
            prune_parents(repo->root, f->path);
        }
        if (f->theirs_file) {
            char theirs_path[1040];
            snprintf(theirs_path, sizeof(theirs_path), "%s.theirs", abs_path);
            localfs_remove(theirs_path);
        }
    }
    vcs_flat_free(&ours_flat);
    vcs_mergestate_free(&st);
    if (!ok) {
        ui_notify(UI_ERROR, "amivcs: not every file could be restored -- the merge state is kept, "
                            "try 'merge --abort' again");
        return 0;
    }
    if (!vcs_index_reset_to(repo->amivcs_dir, &ours_tree)) {
        ui_notify(UI_ERROR, "amivcs: the index could not be written -- the merge state is kept, "
                            "try 'merge --abort' again");
        return 0;
    }
    vcs_mergestate_clear(repo->amivcs_dir);
    vcs_names_forget(repo->amivcs_dir);
    ui_notify(UI_INFO, "Merge aborted.");
    return 1;
}

int vcs_repo_merge_resolve(VcsRepo *repo, const char *path, int take_theirs)
{
    VcsMergeState st;
    VcsMergeFile *f;
    VcsOid tree;
    VcsFlatList flat;
    const VcsFlatEntry *e;
    char abs_path[1024];
    int ok = 1;

    if (!vcs_mergestate_load(repo->amivcs_dir, &st) || !st.active) {
        ui_notify(UI_ERROR, "amivcs: no merge in progress");
        vcs_mergestate_free(&st);
        return 0;
    }
    vcs_repo_learn_names(repo);   /* HEAD, index and the names the merge saved */
    f = vcs_mergestate_find(&st, path);
    if (!f || !vcs_mergestate_is_conflict(f)) {
        ui_notify(UI_ERROR, "amivcs: %s has no merge conflict", vcs_shown(path));
        vcs_mergestate_free(&st);
        return 0;
    }
    if (!vcs_commit_tree(&repo->store, take_theirs ? &st.theirs : &st.ours, &tree)) {
        vcs_mergestate_free(&st);
        return 0;
    }
    vcs_flat_init(&flat);
    if (!vcs_flatten_committed_tree(&repo->store, &tree, "", &flat)) {
        vcs_flat_free(&flat);
        vcs_mergestate_free(&st);
        return 0;
    }
    vcs_flat_sort(&flat);
    e = vcs_flat_find(&flat, path);
    vcs_local_join(abs_path, sizeof(abs_path), repo->root, path);
    if (e) {
        ensure_parent_dir(repo->root, path);
        ok = vcsobjstore_extract_blob_to_file(&repo->store, &e->oid, abs_path)
             && vcs_index_set_blob(repo, path, &e->oid);
    } else {
        if (localfs_exists(abs_path) && !localfs_is_dir(abs_path)) {
            localfs_remove(abs_path);
            prune_parents(repo->root, path);
        }
        ok = vcs_index_remove(repo, path);
    }
    if (ok && f->theirs_file) {
        char theirs_path[1040];
        snprintf(theirs_path, sizeof(theirs_path), "%s.theirs", abs_path);
        localfs_remove(theirs_path);
        f->theirs_file = 0;
    }
    if (ok) {
        f->status = 'R';
        ok = vcs_mergestate_save(repo->amivcs_dir, &st);
    }
    vcs_flat_free(&flat);
    vcs_mergestate_free(&st);
    return ok;
}
