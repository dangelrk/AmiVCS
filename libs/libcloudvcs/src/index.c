/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * index.c -- see index.h. The index is just a VCS_OBJ_TREE OID; add()/
 * remove() rebuild the tree path from the changed leaf up to the root
 * (decode each level with vcs_tree_decode(), upsert/delete the one entry
 * that changed, re-encode+write each level bottom-up with
 * vcs_tree_encode()/vcsobjstore_write()) -- no new object format, no
 * bespoke on-disk layout beyond the one-line "<32-hex-oid>\n" index file
 * itself (same shape as HEAD's detached form, refs.c).
 */
#include "index.h"
#include "mergestate.h"
#include "ignore.h"
#include "treeutil.h"
#include "localfs.h"
#include "vcsname.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int vcs_index_load(const char *amivcs_dir, VcsOid *out_tree_oid)
{
    char path[1024];
    FILE *fp;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_INDEX_FILE);
    fp = fopen(path, "r");
    if (fp) {
        char line[64];
        int ok = 0;
        if (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
            ok = vcs_oid_from_hex(line, out_tree_oid);
        }
        fclose(fp);
        if (ok) return 1;
        /* malformed index file -- fall through to the HEAD fallback below */
    }

    /* No (usable) index file yet: the index mirrors HEAD's committed tree
     * exactly (VCS_OID_ZERO if HEAD has no commits yet), same as a fresh
     * repo or one that predates this feature. Not persisted here -- only
     * an actual add()/remove() materializes the file. */
    {
        VcsObjStore store;
        VcsOid head_commit;

        vcsobjstore_init(&store, amivcs_dir);
        if (!vcs_head_resolve(amivcs_dir, &head_commit)) return 0;
        return vcs_commit_tree(&store, &head_commit, out_tree_oid);
    }
}

int vcs_index_save(const char *amivcs_dir, const VcsOid *tree_oid)
{
    char path[1024], tmp_path[1040], hex[VCS_OID_HEX];
    FILE *fp;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_INDEX_FILE);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    fp = fopen(tmp_path, "w");
    if (!fp) return 0;
    vcs_oid_to_hex(tree_oid, hex);
    if (fprintf(fp, "%s\n", hex) < 0) { fclose(fp); remove(tmp_path); return 0; }
    fclose(fp);

    /* AmigaDOS Rename() fails outright if the destination already
     * exists (see dos.doc's Rename() autodoc) -- unlike POSIX rename(),
     * it never replaces an existing target. Without this, only the
     * very first index save would ever succeed; every add()/remove()
     * after that would silently fail to update the staged tree (see
     * libcloudcore's config_save(), which had the identical bug -- same
     * fix, same reasoning). Delete the old target first; if there isn't
     * one yet (first save), this simply fails harmlessly. */
    remove(path);
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return 0; }
    return 1;
}

int vcs_index_reset_to(const char *amivcs_dir, const VcsOid *tree_oid)
{
    return vcs_index_save(amivcs_dir, tree_oid);
}

/* ------------------------------------------------------------------ */
/* Tree-path rebuild, shared by add() (new_blob != NULL) and remove()
 * (new_blob == NULL). */

static void tree_remove_entry(VcsTree *t, const char *name)
{
    int i, j;
    for (i = 0; i < t->n; i++) {
        if (strcmp(t->items[i].name, name) == 0) {
            for (j = i; j < t->n - 1; j++) t->items[j] = t->items[j + 1];
            t->n--;
            return;
        }
    }
}

static int rebuild_tree(VcsRepo *repo, const VcsOid *cur_tree_oid, const char *relpath,
                        const VcsOid *new_blob_oid, VcsOid *out_tree_oid)
{
    VcsTree tree;
    const char *slash;
    void *data;
    long len;
    int ok;

    vcs_tree_init(&tree);
    if (!vcs_oid_is_zero(cur_tree_oid)) {
        VcsObjType type;
        if (!vcsobjstore_read(&repo->store, cur_tree_oid, &type, &data, &len)) return 0;
        if (type != VCS_OBJ_TREE || !vcs_tree_decode(data, len, &tree)) { free(data); vcs_tree_free(&tree); return 0; }
        free(data);
    }

    slash = strchr(relpath, '/');
    if (!slash) {
        if (new_blob_oid) vcs_tree_upsert(&tree, relpath, 0, new_blob_oid);
        else              tree_remove_entry(&tree, relpath);
    } else {
        char seg[256];
        size_t seglen = (size_t)(slash - relpath);
        const VcsTreeEntry *existing;
        VcsOid child_oid = VCS_OID_ZERO, new_child_oid;

        if (seglen >= sizeof(seg)) { vcs_tree_free(&tree); return 0; }
        memcpy(seg, relpath, seglen);
        seg[seglen] = '\0';

        existing = vcs_tree_find(&tree, seg);
        if (existing && existing->is_dir) child_oid = existing->oid;

        if (!rebuild_tree(repo, &child_oid, slash + 1, new_blob_oid, &new_child_oid)) {
            vcs_tree_free(&tree);
            return 0;
        }

        {
            int child_empty = 1;
            if (!vcs_oid_is_zero(&new_child_oid)) {
                VcsObjType ctype;
                void *cdata;
                long clen;
                if (vcsobjstore_read(&repo->store, &new_child_oid, &ctype, &cdata, &clen)) {
                    VcsTree ctree;
                    vcs_tree_init(&ctree);
                    if (ctype == VCS_OBJ_TREE && vcs_tree_decode(cdata, clen, &ctree)) child_empty = (ctree.n == 0);
                    vcs_tree_free(&ctree);
                    free(cdata);
                }
            }
            if (child_empty) tree_remove_entry(&tree, seg);
            else              vcs_tree_upsert(&tree, seg, 1, &new_child_oid);
        }
    }

    if (!vcs_tree_encode(&tree, &data, &len)) { vcs_tree_free(&tree); return 0; }
    vcs_tree_free(&tree);
    ok = vcsobjstore_write(&repo->store, VCS_OBJ_TREE, data, len, out_tree_oid);
    free(data);
    return ok;
}

/* relpath: repository path (UTF-8, vcsname.h) */
/* 1 if relpath is a <path>.theirs copy the merge in progress wrote --
 * not a file of the user, never staged */
static int merge_theirs_copy(const VcsRepo *repo, const char *relpath)
{
    size_t n = strlen(relpath);
    VcsMergeState st;
    int hit = 0, i;
    if (n <= 7 || strcmp(relpath + n - 7, ".theirs") != 0) return 0;
    if (!vcs_mergestate_active(repo->amivcs_dir) || !vcs_mergestate_load(repo->amivcs_dir, &st)) return 0;
    for (i = 0; i < st.n && !hit; i++)
        hit = st.items[i].theirs_file && strlen(st.items[i].path) == n - 7
              && strncmp(st.items[i].path, relpath, n - 7) == 0;
    vcs_mergestate_free(&st);
    return hit;
}

static int add_file(VcsRepo *repo, const char *relpath)
{
    char abs_path[1024];
    VcsOid blob_oid, cur_tree, new_tree;

    if (merge_theirs_copy(repo, relpath)) return 1;

    if (!vcs_local_join(abs_path, sizeof(abs_path), repo->root, relpath)) return 0;
    if (!vcsobjstore_write_blob_file(&repo->store, abs_path, &blob_oid)) return 0;

    if (!vcs_index_load(repo->amivcs_dir, &cur_tree)) return 0;
    if (!rebuild_tree(repo, &cur_tree, relpath, &blob_oid, &new_tree)) return 0;
    return vcs_index_save(repo->amivcs_dir, &new_tree);
}

/* Recursive directory add is now fully .amivcsignore-aware, same as
 * status()/commit()'s flatten_workdir() -- ignore.c was pulled out of
 * repo.c precisely so this walker could share it (previously it only
 * excluded VCS_DIR_NAME itself; see git history for the old "acceptable
 * v1 gap" note). An explicitly named top-level path is still staged even
 * if ignored -- the user named it, so only the *recursion* into a
 * directory's children filters ignored entries, matching the plan's
 * "close the surprise without -f complexity" call. */
static int add_dir_recursive(VcsRepo *repo, const char *relpath, const VcsIgnore *ig)
{
    char abs_dir[1024];
    LocalDir *dir;
    LocalDirEntry de;
    int ok = 1;

    char rel_local[1024];

    if (vcs_path_to_local(relpath, rel_local, sizeof(rel_local)) < 0) return 0;
    localfs_join(abs_dir, sizeof(abs_dir), repo->root, rel_local);
    dir = localfs_opendir(abs_dir);
    if (!dir) return 0;

    while (ok && localfs_readdir(dir, &de)) {
        char child_local[1024], child_rel[1024];
        if (rel_local[0] == '\0') {
            strncpy(child_local, de.name, sizeof(child_local) - 1);
            child_local[sizeof(child_local) - 1] = '\0';
        } else {
            snprintf(child_local, sizeof(child_local), "%s/%s", rel_local, de.name);
        }
        if (vcs_ignore_match(ig, child_local, de.is_dir)) continue;   /* patterns are local text */
        if (!vcs_path_from_local(child_local, child_rel, sizeof(child_rel))) { ok = 0; break; }
        ok = de.is_dir ? add_dir_recursive(repo, child_rel, ig) : add_file(repo, child_rel);
    }
    localfs_closedir(dir);
    return ok;
}

/* paths of f equal to relpath or below it ("" = all) */
static int under(const char *path, const char *relpath)
{
    size_t n = strlen(relpath);
    if (n == 0) return 1;
    return strncmp(path, relpath, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* index entries below relpath whose file is gone: stage their deletion */
static int stage_missing_below(VcsRepo *repo, const char *relpath)
{
    VcsOid tree;
    VcsFlatList xf;
    int i, ok = 1;
    if (!vcs_index_load(repo->amivcs_dir, &tree)) return 0;
    vcs_flat_init(&xf);
    if (!vcs_flatten_committed_tree(&repo->store, &tree, "", &xf)) { vcs_flat_free(&xf); return 0; }
    for (i = 0; i < xf.n && ok; i++) {
        char abs_path[1024];
        if (!under(xf.items[i].path, relpath)) continue;
        if (vcs_local_join(abs_path, sizeof(abs_path), repo->root, xf.items[i].path)
            && !localfs_exists(abs_path))
            ok = vcs_index_remove(repo, xf.items[i].path);
    }
    vcs_flat_free(&xf);
    return ok;
}

static int index_add(VcsRepo *repo, const char *relpath);

/* during a merge, adding a conflicting path marks it resolved (git) */
int vcs_index_add(VcsRepo *repo, const char *relpath)
{
    if (!index_add(repo, relpath)) return 0;
    return vcs_mergestate_mark_resolved(repo->amivcs_dir, relpath);
}

static int index_add(VcsRepo *repo, const char *relpath)
{
    char abs_path[1024];

    /* the caller's name table maps relpath (vcs_repo_status_raw() or
     * vcs_repo_learn_names() before it, vcsname.h) */
    if (!vcs_local_join(abs_path, sizeof(abs_path), repo->root, relpath)) return 0;
    if (localfs_is_dir(abs_path)) {
        VcsIgnore ig;
        int ok;
        vcs_ignore_load(repo->root, &ig);
        ok = add_dir_recursive(repo, relpath, &ig);
        vcs_ignore_free(&ig);
        if (ok) ok = stage_missing_below(repo, relpath);
        return ok;
    }
    if (!localfs_exists(abs_path)) {
        /* gone from disk: stage its deletion (a file, or everything the
         * index has below a deleted directory) */
        VcsOid tree;
        VcsFlatList xf;
        int i, found = 0, ok = 1;
        if (!vcs_index_load(repo->amivcs_dir, &tree)) return 0;
        vcs_flat_init(&xf);
        if (!vcs_flatten_committed_tree(&repo->store, &tree, "", &xf)) { vcs_flat_free(&xf); return 0; }
        for (i = 0; i < xf.n && ok; i++)
            if (under(xf.items[i].path, relpath)) { found = 1; ok = vcs_index_remove(repo, xf.items[i].path); }
        vcs_flat_free(&xf);
        if (!found && ok) {
            /* a conflict settled by keeping the file deleted: nothing to
             * stage, but it is resolved (vcs_index_add() marks it) */
            VcsMergeState st;
            if (vcs_mergestate_active(repo->amivcs_dir) && vcs_mergestate_load(repo->amivcs_dir, &st)) {
                VcsMergeFile *f = vcs_mergestate_find(&st, relpath);
                found = f && vcs_mergestate_is_conflict(f);
                vcs_mergestate_free(&st);
            }
        }
        return found && ok;
    }
    return add_file(repo, relpath);
}

int vcs_index_set_blob(VcsRepo *repo, const char *relpath, const VcsOid *blob_oid)
{
    VcsOid cur_tree, new_tree;
    if (!vcs_index_load(repo->amivcs_dir, &cur_tree)) return 0;
    if (!rebuild_tree(repo, &cur_tree, relpath, blob_oid, &new_tree)) return 0;
    return vcs_index_save(repo->amivcs_dir, &new_tree);
}

int vcs_index_unstage(VcsRepo *repo, const char *relpath)
{
    VcsOid head_commit, head_tree, index_tree;
    VcsFlatList hf, xf;
    int i, ok = 1;

    head_tree = VCS_OID_ZERO;
    if (vcs_head_resolve(repo->amivcs_dir, &head_commit) && !vcs_oid_is_zero(&head_commit)
        && !vcs_commit_tree(&repo->store, &head_commit, &head_tree)) return 0;
    if (!vcs_index_load(repo->amivcs_dir, &index_tree)) return 0;
    vcs_flat_init(&hf);
    vcs_flat_init(&xf);
    if (!vcs_flatten_committed_tree(&repo->store, &head_tree, "", &hf)
        || !vcs_flatten_committed_tree(&repo->store, &index_tree, "", &xf)) {
        vcs_flat_free(&hf);
        vcs_flat_free(&xf);
        return 0;
    }
    vcs_flat_sort(&hf);
    vcs_flat_sort(&xf);
    /* staged additions/changes under relpath: back to HEAD or out */
    for (i = 0; i < xf.n && ok; i++) {
        const VcsFlatEntry *h;
        if (!under(xf.items[i].path, relpath)) continue;
        h = vcs_flat_find(&hf, xf.items[i].path);
        if (!h) ok = vcs_index_remove(repo, xf.items[i].path);
        else if (!vcs_oid_equal(&h->oid, &xf.items[i].oid)) ok = vcs_index_set_blob(repo, h->path, &h->oid);
    }
    /* staged deletions under relpath: HEAD's entry back */
    for (i = 0; i < hf.n && ok; i++)
        if (under(hf.items[i].path, relpath) && !vcs_flat_find(&xf, hf.items[i].path))
            ok = vcs_index_set_blob(repo, hf.items[i].path, &hf.items[i].oid);
    vcs_flat_free(&hf);
    vcs_flat_free(&xf);
    return ok;
}

int vcs_index_remove(VcsRepo *repo, const char *relpath)
{
    VcsOid cur_tree, new_tree;

    if (!vcs_index_load(repo->amivcs_dir, &cur_tree)) return 0;
    if (!rebuild_tree(repo, &cur_tree, relpath, NULL, &new_tree)) return 0;
    return vcs_index_save(repo->amivcs_dir, &new_tree);
}

int vcs_index_add_all(VcsRepo *repo)
{
    VcsStatus st;
    int i, ok = 1;

    if (!vcs_repo_status_raw(repo, &st)) return 0;

    for (i = 0; i < st.unstaged.n && ok; i++) {
        const VcsChange *c = &st.unstaged.items[i];
        ok = (c->kind == 'D') ? vcs_index_remove(repo, c->path) && vcs_mergestate_mark_resolved(repo->amivcs_dir, c->path)
                              : vcs_index_add(repo, c->path);
    }

    vcs_status_free(&st);
    return ok;
}
