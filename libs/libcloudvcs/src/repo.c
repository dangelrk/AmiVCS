/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * repo.c -- see repo.h. Two independent tree walks are used on purpose:
 *
 *   - snapshot_dir()/vcs_repo_snapshot_workdir() builds real, persisted
 *     VCS_OBJ_TREE objects bottom-up (needed by commit() to actually
 *     store the new state).
 *   - flatten_workdir()/flatten_committed_tree() instead produce a flat,
 *     sorted (path -> oid) listing without writing anything, purely for
 *     comparing two trees (status()/is_dirty()).
 *
 * Keeping these separate avoids a shared "maybe persist, maybe not" flag
 * threaded through one recursive builder -- status (called often, e.g.
 * before every commit) never touches the object store at all.
 */
#include "repo.h"
#include "index.h"
#include "ignore.h"
#include "treeutil.h"
#include "oidset.h"
#include "localfs.h"
#include "md5.h"
#include "ui_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *DEFAULT_IGNORE =
    "# AMIVCS default ignore patterns\n"
    ".amivcs/\n"
    "#?.o\n"
    "#?.lib\n"
    "bin/\n"
    "obj/\n";

int vcs_repo_init(const char *root)
{
    char amivcs_dir[1024];
    VcsObjStore store;
    VcsRefList empty_refs;

    localfs_join(amivcs_dir, sizeof(amivcs_dir), root, VCS_DIR_NAME);
    if (localfs_exists(amivcs_dir)) return 0; /* already a repo */

    if (!localfs_mkdir_p(amivcs_dir)) return 0;

    vcsobjstore_init(&store, amivcs_dir);
    if (!vcsobjstore_create_dirs(&store)) return 0;

    empty_refs.items = NULL;
    empty_refs.n = empty_refs.cap = 0;
    if (!vcs_refs_save(amivcs_dir, &empty_refs)) return 0;

    if (!vcs_head_set_branch(amivcs_dir, "main")) return 0;

    {
        char ignore_path[1024];
        localfs_join(ignore_path, sizeof(ignore_path), root, VCS_IGNORE_FILE);
        if (!localfs_exists(ignore_path)) {
            FILE *fp = fopen(ignore_path, "w");
            if (fp) {
                fputs(DEFAULT_IGNORE, fp);
                fclose(fp);
            }
        }
    }

    return 1;
}

int vcs_repo_open(VcsRepo *repo, const char *root)
{
    strncpy(repo->root, root, sizeof(repo->root) - 1);
    repo->root[sizeof(repo->root) - 1] = '\0';

    localfs_join(repo->amivcs_dir, sizeof(repo->amivcs_dir), root, VCS_DIR_NAME);
    if (!localfs_is_dir(repo->amivcs_dir)) return 0;

    vcsobjstore_init(&repo->store, repo->amivcs_dir);
    return 1;
}

/* ------------------------------------------------------------------ */
/* .amivcsignore matching itself now lives in ignore.c/ignore.h (shared
 * with index.c's add_dir_recursive()); join_rel() is a small path-
 * building helper local to this file's three walkers. */

static void join_rel(char *out, size_t sz, const char *prefix, const char *name)
{
    if (prefix[0] == '\0') {
        strncpy(out, name, sz - 1);
        out[sz - 1] = '\0';
    } else {
        snprintf(out, sz, "%s/%s", prefix, name);
    }
}

/* ------------------------------------------------------------------ */
/* Persisting snapshot builder (commit()).                              */

static int snapshot_dir(VcsRepo *repo, const char *abs_dir, const char *rel_prefix,
                        VcsIgnore *ig, VcsOid *out_oid)
{
    LocalDir *dir;
    LocalDirEntry de;
    VcsTree tree;
    void *data;
    long len;
    int ok = 1;

    vcs_tree_init(&tree);

    dir = localfs_opendir(abs_dir);
    if (!dir) { vcs_tree_free(&tree); return 0; }

    while (localfs_readdir(dir, &de)) {
        char abs_path[1024], rel_path[1024];
        VcsOid oid;

        join_rel(rel_path, sizeof(rel_path), rel_prefix, de.name);
        if (vcs_ignore_match(ig, rel_path, de.is_dir)) continue;
        localfs_join(abs_path, sizeof(abs_path), abs_dir, de.name);

        if (de.is_dir) {
            if (!snapshot_dir(repo, abs_path, rel_path, ig, &oid)) { ok = 0; break; }
        } else {
            if (!vcsobjstore_write_blob_file(&repo->store, abs_path, &oid)) { ok = 0; break; }
        }
        vcs_tree_upsert(&tree, de.name, de.is_dir, &oid);
    }
    localfs_closedir(dir);

    if (!ok) { vcs_tree_free(&tree); return 0; }

    if (!vcs_tree_encode(&tree, &data, &len)) { vcs_tree_free(&tree); return 0; }
    vcs_tree_free(&tree);

    ok = vcsobjstore_write(&repo->store, VCS_OBJ_TREE, data, len, out_oid);
    free(data);
    return ok;
}

int vcs_repo_snapshot_workdir(VcsRepo *repo, VcsOid *out_tree_oid)
{
    VcsIgnore ig;
    int ok;

    vcs_ignore_load(repo->root, &ig);
    ok = snapshot_dir(repo, repo->root, "", &ig, out_tree_oid);
    vcs_ignore_free(&ig);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Working-tree flattening, read-only, for status()/is_dirty(). The
 * committed-tree side (VcsFlatList, vcs_flatten_committed_tree()) is
 * shared with merge.c via treeutil.h. */

static int flatten_workdir(const char *abs_root, const char *abs_dir, const char *rel_prefix,
                           VcsIgnore *ig, VcsFlatList *out)
{
    LocalDir *dir;
    LocalDirEntry de;

    (void)abs_root;
    dir = localfs_opendir(abs_dir);
    if (!dir) return 0;

    while (localfs_readdir(dir, &de)) {
        char abs_path[1024], rel_path[1024];

        join_rel(rel_path, sizeof(rel_path), rel_prefix, de.name);
        if (vcs_ignore_match(ig, rel_path, de.is_dir)) continue;
        localfs_join(abs_path, sizeof(abs_path), abs_dir, de.name);

        if (de.is_dir) {
            if (!flatten_workdir(abs_root, abs_path, rel_path, ig, out)) { localfs_closedir(dir); return 0; }
        } else {
            char hex[VCS_OID_HEX];
            VcsOid oid;
            if (!md5_file_hex(abs_path, hex)) { localfs_closedir(dir); return 0; }
            if (!vcs_oid_from_hex(hex, &oid)) { localfs_closedir(dir); return 0; }
            vcs_flat_append(out, rel_path, &oid);
        }
    }
    localfs_closedir(dir);
    return 1;
}

static int resolve_head_tree(VcsRepo *repo, VcsOid *out_tree)
{
    VcsOid head_commit;

    if (!vcs_head_resolve(repo->amivcs_dir, &head_commit)) return 0;
    return vcs_commit_tree(&repo->store, &head_commit, out_tree);
}

/* Diffs two sorted flat lists via a simple two-way merge scan (k=2
 * variant of the same idea as libcloudsync's k-way SyncIndex scan). cb
 * receives ('N'ew / 'M'odified / 'D'eleted, path) for each difference;
 * cb may be NULL to just count (is_dirty()). Returns the number of
 * differences found. */
static int diff_flat(const VcsFlatList *head, const VcsFlatList *work,
                     void (*cb)(char kind, const char *path, void *ctx), void *ctx)
{
    int hi = 0, wi = 0, changes = 0;

    while (hi < head->n || wi < work->n) {
        int cmp;
        if (hi >= head->n) cmp = 1;
        else if (wi >= work->n) cmp = -1;
        else cmp = strcmp(head->items[hi].path, work->items[wi].path);

        if (cmp == 0) {
            if (!vcs_oid_equal(&head->items[hi].oid, &work->items[wi].oid)) {
                changes++;
                if (cb) cb('M', head->items[hi].path, ctx);
            }
            hi++; wi++;
        } else if (cmp < 0) {
            changes++;
            if (cb) cb('D', head->items[hi].path, ctx);
            hi++;
        } else {
            changes++;
            if (cb) cb('N', work->items[wi].path, ctx);
            wi++;
        }
    }
    return changes;
}

static void collect_change_cb(char kind, const char *path, void *ctx)
{
    VcsChangeList *list = (VcsChangeList *)ctx;
    VcsChange *c;

    if (list->n == list->cap) {
        int newcap = list->cap ? list->cap * 2 : 16;
        VcsChange *tmp = (VcsChange *)realloc(list->items, sizeof(VcsChange) * (size_t)newcap);
        if (!tmp) return;
        list->items = tmp;
        list->cap = newcap;
    }
    c = &list->items[list->n++];
    c->kind = kind;
    strncpy(c->path, path, sizeof(c->path) - 1);
    c->path[sizeof(c->path) - 1] = '\0';
}

void vcs_status_free(VcsStatus *out)
{
    free(out->staged.items);
    free(out->unstaged.items);
    out->staged.items = out->unstaged.items = NULL;
    out->staged.n = out->staged.cap = out->unstaged.n = out->unstaged.cap = 0;
}

int vcs_repo_status_raw(VcsRepo *repo, VcsStatus *out)
{
    VcsOid head_tree, index_tree;
    VcsFlatList head_flat, index_flat, work_flat;
    VcsIgnore ig;

    out->staged.items = out->unstaged.items = NULL;
    out->staged.n = out->staged.cap = out->unstaged.n = out->unstaged.cap = 0;

    if (!resolve_head_tree(repo, &head_tree)) return 0;
    if (!vcs_index_load(repo->amivcs_dir, &index_tree)) return 0;

    vcs_flat_init(&head_flat);
    vcs_flat_init(&index_flat);
    if (!vcs_flatten_committed_tree(&repo->store, &head_tree, "", &head_flat)
        || !vcs_flatten_committed_tree(&repo->store, &index_tree, "", &index_flat)) {
        vcs_flat_free(&head_flat);
        vcs_flat_free(&index_flat);
        return 0;
    }
    vcs_flat_sort(&head_flat);
    vcs_flat_sort(&index_flat);

    vcs_ignore_load(repo->root, &ig);
    vcs_flat_init(&work_flat);
    if (!flatten_workdir(repo->root, repo->root, "", &ig, &work_flat)) {
        vcs_ignore_free(&ig);
        vcs_flat_free(&head_flat);
        vcs_flat_free(&index_flat);
        vcs_flat_free(&work_flat);
        return 0;
    }
    vcs_ignore_free(&ig);
    vcs_flat_sort(&work_flat);

    diff_flat(&head_flat, &index_flat, collect_change_cb, &out->staged);
    diff_flat(&index_flat, &work_flat, collect_change_cb, &out->unstaged);

    vcs_flat_free(&head_flat);
    vcs_flat_free(&index_flat);
    vcs_flat_free(&work_flat);
    return 1;
}

static void print_change_list(const VcsChangeList *list)
{
    int i;
    for (i = 0; i < list->n; i++) {
        const char *label = list->items[i].kind == 'N' ? "new"
                           : list->items[i].kind == 'M' ? "modified" : "deleted";
        printf("  %-9s %s\n", label, list->items[i].path);
    }
}

int vcs_repo_status(VcsRepo *repo)
{
    VcsStatus st;

    if (!vcs_repo_status_raw(repo, &st)) return 0;

    if (st.staged.n > 0) {
        printf("Changes to be committed:\n");
        print_change_list(&st.staged);
    }
    if (st.unstaged.n > 0) {
        printf("Changes not staged for commit:\n");
        print_change_list(&st.unstaged);
    }
    if (st.staged.n == 0 && st.unstaged.n == 0) printf("nothing to commit, working tree clean\n");

    vcs_status_free(&st);
    return 1;
}

int vcs_repo_is_dirty(VcsRepo *repo)
{
    VcsOid head_tree;
    VcsFlatList head_flat, work_flat;
    VcsIgnore ig;
    int changes;

    if (!resolve_head_tree(repo, &head_tree)) return -1;

    vcs_flat_init(&head_flat);
    if (!vcs_flatten_committed_tree(&repo->store, &head_tree, "", &head_flat)) {
        vcs_flat_free(&head_flat);
        return -1;
    }
    vcs_flat_sort(&head_flat);

    vcs_ignore_load(repo->root, &ig);
    vcs_flat_init(&work_flat);
    if (!flatten_workdir(repo->root, repo->root, "", &ig, &work_flat)) {
        vcs_ignore_free(&ig);
        vcs_flat_free(&head_flat);
        vcs_flat_free(&work_flat);
        return -1;
    }
    vcs_ignore_free(&ig);
    vcs_flat_sort(&work_flat);

    changes = diff_flat(&head_flat, &work_flat, NULL, NULL);

    vcs_flat_free(&head_flat);
    vcs_flat_free(&work_flat);
    return changes != 0;
}

/* ------------------------------------------------------------------ */
/* Materialize a committed tree into the working directory (checkout/    */
/* switch, and later merge's fast-forward path).                        */

static int materialize_dir(VcsRepo *repo, const char *abs_dir, const char *rel_prefix,
                           const VcsOid *tree_oid, VcsIgnore *ig)
{
    VcsObjType type;
    void *data;
    long len;
    VcsTree tree;
    int i, ok = 1;
    LocalDir *dir;
    LocalDirEntry de;

    vcs_tree_init(&tree);
    if (!vcs_oid_is_zero(tree_oid)) {
        if (!vcsobjstore_read(&repo->store, tree_oid, &type, &data, &len)) return 0;
        if (type != VCS_OBJ_TREE || !vcs_tree_decode(data, len, &tree)) { free(data); return 0; }
        free(data);
    }

    /* Pass 1: remove anything on disk that isn't in the tree and isn't
     * ignored (an ignored path, e.g. build output, is simply left alone). */
    dir = localfs_opendir(abs_dir);
    if (dir) {
        while (localfs_readdir(dir, &de)) {
            char rel_path[1024];
            join_rel(rel_path, sizeof(rel_path), rel_prefix, de.name);
            if (vcs_ignore_match(ig, rel_path, de.is_dir)) continue;
            if (vcs_tree_find(&tree, de.name)) continue;
            {
                char abs_path[1024];
                localfs_join(abs_path, sizeof(abs_path), abs_dir, de.name);
                if (de.is_dir) {
                    VcsOid empty = VCS_OID_ZERO;
                    if (!materialize_dir(repo, abs_path, rel_path, &empty, ig)) { ok = 0; break; }
                    localfs_remove(abs_path);
                } else {
                    localfs_remove(abs_path);
                }
            }
        }
        localfs_closedir(dir);
    }
    if (!ok) { vcs_tree_free(&tree); return 0; }

    /* Pass 2: create/update everything the tree calls for. */
    for (i = 0; i < tree.n; i++) {
        char abs_path[1024], rel_path[1024];
        localfs_join(abs_path, sizeof(abs_path), abs_dir, tree.items[i].name);
        join_rel(rel_path, sizeof(rel_path), rel_prefix, tree.items[i].name);

        if (tree.items[i].is_dir) {
            if (!localfs_is_dir(abs_path) && !localfs_mkdir(abs_path)) { ok = 0; break; }
            if (!materialize_dir(repo, abs_path, rel_path, &tree.items[i].oid, ig)) { ok = 0; break; }
        } else {
            if (!vcsobjstore_extract_blob_to_file(&repo->store, &tree.items[i].oid, abs_path)) { ok = 0; break; }
        }
    }

    vcs_tree_free(&tree);
    return ok;
}

int vcs_repo_materialize_tree(VcsRepo *repo, const VcsOid *tree_oid)
{
    VcsIgnore ig;
    int ok;
    vcs_ignore_load(repo->root, &ig);
    ok = materialize_dir(repo, repo->root, "", tree_oid, &ig);
    vcs_ignore_free(&ig);
    return ok;
}

/* ------------------------------------------------------------------ */

int vcs_repo_commit(VcsRepo *repo, const char *author, const char *message, int stage_all, VcsOid *out_oid)
{
    VcsHead head;
    VcsOid parent_oid, tree_oid;
    VcsCommit commit;
    void *data;
    long len;
    VcsOid commit_oid;

    if (!vcs_head_load(repo->amivcs_dir, &head)) return 0;
    if (head.detached) { ui_notify(UI_ERROR, "amivcs: HEAD is detached -- switch to a branch first"); return 0; }

    if (!vcs_head_resolve(repo->amivcs_dir, &parent_oid)) return 0;

    if (stage_all && !vcs_index_add_all(repo)) return 0;
    if (!vcs_index_load(repo->amivcs_dir, &tree_oid)) return 0;

    if (!vcs_oid_is_zero(&parent_oid)) {
        VcsOid parent_tree;
        if (!resolve_head_tree(repo, &parent_tree)) return 0;
        if (vcs_oid_equal(&parent_tree, &tree_oid)) {
            ui_notify(UI_INFO, "nothing to commit, working tree clean");
            return 0;
        }
    }

    vcs_commit_init(&commit);
    commit.tree = tree_oid;
    if (!vcs_oid_is_zero(&parent_oid)) {
        commit.parents[0] = parent_oid;
        commit.nparents = 1;
    }
    commit.mtime = (long)time(NULL);
    strncpy(commit.author, (author && author[0]) ? author : "unknown", sizeof(commit.author) - 1);
    commit.author[sizeof(commit.author) - 1] = '\0';
    commit.message = (char *)malloc(strlen(message ? message : "") + 1);
    if (!commit.message) { vcs_commit_free(&commit); return 0; }
    strcpy(commit.message, message ? message : "");

    if (!vcs_commit_encode(&commit, &data, &len)) { vcs_commit_free(&commit); return 0; }
    vcs_commit_free(&commit);

    if (!vcsobjstore_write(&repo->store, VCS_OBJ_COMMIT, data, len, &commit_oid)) { free(data); return 0; }
    free(data);

    {
        VcsRefList refs;
        char fullname[128];
        if (!vcs_refs_load(repo->amivcs_dir, &refs)) return 0;
        snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, head.branch);
        vcs_refs_upsert(&refs, fullname, &commit_oid);
        if (!vcs_refs_save(repo->amivcs_dir, &refs)) { vcs_refs_free(&refs); return 0; }
        vcs_refs_free(&refs);
    }

    if (out_oid) *out_oid = commit_oid;
    return 1;
}

static int read_commit(const VcsObjStore *store, const VcsOid *oid, VcsCommit *out)
{
    VcsObjType type;
    void *data;
    long len;

    vcs_commit_init(out);
    if (vcs_oid_is_zero(oid)) return 0;
    if (!vcsobjstore_read(store, oid, &type, &data, &len)) return 0;
    if (type != VCS_OBJ_COMMIT || !vcs_commit_decode(data, len, out)) { free(data); return 0; }
    free(data);
    return 1;
}

void vcs_loglist_free(VcsLogList *list)
{
    int i;
    for (i = 0; i < list->n; i++) free(list->items[i].message);
    free(list->items);
    list->items = NULL;
    list->n = list->cap = 0;
}

static void log_list_append(VcsLogList *list, const VcsOid *oid, const VcsCommit *c)
{
    VcsLogEntry *e;
    int i;

    if (list->n == list->cap) {
        int newcap = list->cap ? list->cap * 2 : 32;
        VcsLogEntry *tmp = (VcsLogEntry *)realloc(list->items, sizeof(VcsLogEntry) * (size_t)newcap);
        if (!tmp) return;
        list->items = tmp;
        list->cap = newcap;
    }
    e = &list->items[list->n++];
    e->oid = *oid;
    e->nparents = c->nparents;
    for (i = 0; i < c->nparents; i++) e->parents[i] = c->parents[i];
    e->mtime = c->mtime;
    strncpy(e->author, c->author, sizeof(e->author) - 1);
    e->author[sizeof(e->author) - 1] = '\0';
    e->message = (char *)malloc(strlen(c->message ? c->message : "") + 1);
    if (e->message) strcpy(e->message, c->message ? c->message : "");
    e->decoration[0] = '\0';
}

/* Follows first-parent only, so a merge commit's log shows just the
 * branch it was merged INTO, not the incoming side too -- the CLI's
 * plain `log` stays console-narrow/first-parent by design (see
 * repo.h) -- vcs_repo_log_all_raw() below is the full-DAG data source
 * for the GUI's history graph. */
int vcs_repo_log_raw(VcsRepo *repo, VcsLogList *out)
{
    VcsOid cur;

    out->items = NULL;
    out->n = out->cap = 0;

    if (!vcs_head_resolve(repo->amivcs_dir, &cur)) return 0;

    while (!vcs_oid_is_zero(&cur)) {
        VcsCommit c;
        VcsOid next;

        if (!read_commit(&repo->store, &cur, &c)) { vcs_loglist_free(out); return 0; }
        log_list_append(out, &cur, &c);
        next = c.nparents > 0 ? c.parents[0] : VCS_OID_ZERO;
        vcs_commit_free(&c);
        cur = next;
    }
    return 1;
}

/* Frontier for the date-order walk below: a small max-priority-queue by
 * mtime (linear-scan pop, same "small enough, skip the hash table/heap"
 * idiom as VcsOidSet/VcsFlatList) -- ties broken by OID for determinism
 * when two commits share a timestamp (coarse clocks, scripted commits
 * within the same second). */
typedef struct { VcsOid oid; long mtime; } VcsFrontierItem;
typedef struct { VcsFrontierItem *items; int n, cap; } VcsFrontier;

static void frontier_free(VcsFrontier *f) { free(f->items); f->items = NULL; f->n = f->cap = 0; }

static void frontier_push(VcsFrontier *f, const VcsOid *oid, long mtime)
{
    if (f->n == f->cap) {
        int newcap = f->cap ? f->cap * 2 : 16;
        VcsFrontierItem *tmp = (VcsFrontierItem *)realloc(f->items, sizeof(VcsFrontierItem) * (size_t)newcap);
        if (!tmp) return;
        f->items = tmp;
        f->cap = newcap;
    }
    f->items[f->n].oid = *oid;
    f->items[f->n].mtime = mtime;
    f->n++;
}

static int frontier_pop_max(VcsFrontier *f, VcsOid *out_oid)
{
    int i, best;
    if (f->n == 0) return 0;
    best = 0;
    for (i = 1; i < f->n; i++) {
        if (f->items[i].mtime > f->items[best].mtime
            || (f->items[i].mtime == f->items[best].mtime
                && memcmp(f->items[i].oid.b, f->items[best].oid.b, VCS_OID_SIZE) > 0)) {
            best = i;
        }
    }
    *out_oid = f->items[best].oid;
    f->items[best] = f->items[f->n - 1];
    f->n--;
    return 1;
}

/* A commit only ever enters the frontier once something that's already
 * been emitted (a ref tip, or a child commit) names it as a parent --
 * "queued" gates that, so no parent can be popped before ALL of the
 * children that reference it have themselves been emitted first. Popping
 * the highest-mtime frontier entry each step then gives "git log --all
 * --date-order"'s guarantee: newest-first, but never a parent before its
 * child. (Two independently-diverged tips sharing a timestamp can still
 * only be ordered by the OID tie-break in frontier_pop_max() -- no
 * causal relationship exists to prefer one over the other.) */
int vcs_repo_log_all_raw(VcsRepo *repo, VcsLogList *out)
{
    VcsRefList refs;
    VcsFrontier front;
    VcsOidSet visited, queued;
    VcsOid cur;
    int i;

    out->items = NULL;
    out->n = out->cap = 0;

    if (!vcs_refs_load(repo->amivcs_dir, &refs)) return 0;

    front.items = NULL;
    front.n = front.cap = 0;
    vcs_oidset_init(&visited);
    vcs_oidset_init(&queued);

    for (i = 0; i < refs.n; i++) {
        VcsCommit c;
        if (vcs_oid_is_zero(&refs.items[i].oid) || vcs_oidset_contains(&queued, &refs.items[i].oid)) continue;
        if (!read_commit(&repo->store, &refs.items[i].oid, &c)) continue;
        frontier_push(&front, &refs.items[i].oid, c.mtime);
        vcs_oidset_add(&queued, &refs.items[i].oid);
        vcs_commit_free(&c);
    }

    while (frontier_pop_max(&front, &cur)) {
        VcsCommit c;
        int p;

        if (vcs_oidset_contains(&visited, &cur)) continue;
        vcs_oidset_add(&visited, &cur);

        if (!read_commit(&repo->store, &cur, &c)) continue; /* unreadable: skip, don't fail the whole walk */
        log_list_append(out, &cur, &c);

        for (p = 0; p < c.nparents; p++) {
            VcsCommit pc;
            if (vcs_oidset_contains(&queued, &c.parents[p])) continue;
            if (!read_commit(&repo->store, &c.parents[p], &pc)) continue;
            frontier_push(&front, &c.parents[p], pc.mtime);
            vcs_oidset_add(&queued, &c.parents[p]);
            vcs_commit_free(&pc);
        }
        vcs_commit_free(&c);
    }
    frontier_free(&front);
    vcs_oidset_free(&queued);
    vcs_oidset_free(&visited);

    /* Decorate: for each branch tip, append its short name to the
     * matching entry's decoration (a tip commit may be shared by several
     * branches, hence ", "-joined rather than overwritten). */
    for (i = 0; i < refs.n; i++) {
        int j;
        if (vcs_oid_is_zero(&refs.items[i].oid)) continue;
        for (j = 0; j < out->n; j++) {
            if (vcs_oid_equal(&out->items[j].oid, &refs.items[i].oid)) {
                const char *short_name = refs.items[i].name + strlen(VCS_REF_PREFIX);
                char *deco = out->items[j].decoration;
                size_t curlen = strlen(deco);
                if (curlen > 0 && curlen + 2 < sizeof(out->items[j].decoration)) strcat(deco, ", ");
                strncat(deco, short_name, sizeof(out->items[j].decoration) - strlen(deco) - 1);
                break;
            }
        }
    }
    vcs_refs_free(&refs);
    return 1;
}

int vcs_repo_log(VcsRepo *repo)
{
    VcsLogList log;
    int i;

    if (!vcs_repo_log_raw(repo, &log)) return 0;
    if (log.n == 0) { printf("(no commits yet)\n"); vcs_loglist_free(&log); return 1; }

    for (i = 0; i < log.n; i++) {
        char hex[VCS_OID_HEX];
        time_t t;
        char timebuf[32];

        vcs_oid_to_hex(&log.items[i].oid, hex);
        t = (time_t)log.items[i].mtime;
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", gmtime(&t));

        printf("commit %s\n", hex);
        printf("Author: %s\n", log.items[i].author);
        printf("Date:   %s UTC\n", timebuf);
        printf("\n    %s\n\n", log.items[i].message ? log.items[i].message : "");
    }
    vcs_loglist_free(&log);
    return 1;
}

int vcs_repo_branch(VcsRepo *repo, const char *name)
{
    VcsRefList refs;
    VcsHead head;

    if (!vcs_refs_load(repo->amivcs_dir, &refs)) return 0;

    if (!name) {
        int i;
        vcs_head_load(repo->amivcs_dir, &head);
        for (i = 0; i < refs.n; i++) {
            const char *short_name = refs.items[i].name + strlen(VCS_REF_PREFIX);
            int is_current = !head.detached && strcmp(short_name, head.branch) == 0;
            printf("%s %s\n", is_current ? "*" : " ", short_name);
        }
        vcs_refs_free(&refs);
        return 1;
    }

    {
        char fullname[128];
        VcsOid head_commit;

        snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, name);
        if (vcs_refs_find(&refs, fullname)) {
            ui_notify(UI_ERROR, "amivcs: branch '%s' already exists", name);
            vcs_refs_free(&refs);
            return 0;
        }
        if (!vcs_head_resolve(repo->amivcs_dir, &head_commit)) { vcs_refs_free(&refs); return 0; }
        if (vcs_oid_is_zero(&head_commit)) {
            ui_notify(UI_ERROR, "amivcs: cannot branch -- no commits yet");
            vcs_refs_free(&refs);
            return 0;
        }
        vcs_refs_upsert(&refs, fullname, &head_commit);
        if (!vcs_refs_save(repo->amivcs_dir, &refs)) { vcs_refs_free(&refs); return 0; }
    }
    vcs_refs_free(&refs);
    return 1;
}

int vcs_repo_switch(VcsRepo *repo, const char *branch)
{
    VcsRefList refs;
    VcsRef *r;
    char fullname[128];
    int dirty;

    dirty = vcs_repo_is_dirty(repo);
    if (dirty != 0) {
        ui_notify(UI_ERROR, dirty < 0
            ? "amivcs: could not check working tree state"
            : "amivcs: working tree has uncommitted changes -- commit first");
        return 0;
    }

    if (!vcs_refs_load(repo->amivcs_dir, &refs)) return 0;
    snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, branch);
    r = vcs_refs_find(&refs, fullname);
    if (!r) {
        ui_notify(UI_ERROR, "amivcs: no such branch '%s'", branch);
        vcs_refs_free(&refs);
        return 0;
    }

    {
        VcsOid tree_oid;
        if (!vcs_commit_tree(&repo->store, &r->oid, &tree_oid)) { vcs_refs_free(&refs); return 0; }
        if (!vcs_repo_materialize_tree(repo, &tree_oid)) { vcs_refs_free(&refs); return 0; }
        vcs_index_reset_to(repo->amivcs_dir, &tree_oid);
    }

    vcs_refs_free(&refs);
    return vcs_head_set_branch(repo->amivcs_dir, branch);
}

int vcs_repo_checkout(VcsRepo *repo)
{
    VcsOid head_tree;
    int dirty = vcs_repo_is_dirty(repo);

    if (dirty != 0) {
        ui_notify(UI_ERROR, dirty < 0
            ? "amivcs: could not check working tree state"
            : "amivcs: working tree has uncommitted changes -- commit first");
        return 0;
    }

    if (!resolve_head_tree(repo, &head_tree)) return 0;
    if (!vcs_repo_materialize_tree(repo, &head_tree)) return 0;
    vcs_index_reset_to(repo->amivcs_dir, &head_tree);
    return 1;
}
