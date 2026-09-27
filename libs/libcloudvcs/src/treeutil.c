/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "treeutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vcs_flat_init(VcsFlatList *f) { f->items = NULL; f->n = f->cap = 0; }

void vcs_flat_free(VcsFlatList *f)
{
    int i;
    for (i = 0; i < f->n; i++) free(f->items[i].path);
    free(f->items);
    f->items = NULL;
    f->n = f->cap = 0;
}

void vcs_flat_append(VcsFlatList *f, const char *path, const VcsOid *oid)
{
    if (f->n == f->cap) {
        int newcap = f->cap ? f->cap * 2 : 64;
        VcsFlatEntry *tmp = (VcsFlatEntry *)realloc(f->items, sizeof(VcsFlatEntry) * (size_t)newcap);
        if (!tmp) return;
        f->items = tmp;
        f->cap = newcap;
    }
    {
        size_t plen = strlen(path) + 1;
        f->items[f->n].path = (char *)malloc(plen);
        if (!f->items[f->n].path) return;
        memcpy(f->items[f->n].path, path, plen);
        f->items[f->n].oid = *oid;
        f->n++;
    }
}

static int flat_cmp(const void *a, const void *b)
{
    return strcmp(((const VcsFlatEntry *)a)->path, ((const VcsFlatEntry *)b)->path);
}

void vcs_flat_sort(VcsFlatList *f)
{
    if (f->n > 1) qsort(f->items, (size_t)f->n, sizeof(VcsFlatEntry), flat_cmp);
}

const VcsFlatEntry *vcs_flat_find(const VcsFlatList *f, const char *path)
{
    int lo = 0, hi = f->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(f->items[mid].path, path);
        if (cmp == 0) return &f->items[mid];
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

int vcs_commit_tree(const VcsObjStore *store, const VcsOid *commit_oid, VcsOid *out_tree)
{
    VcsObjType type;
    void *data;
    long len;
    VcsCommit c;

    if (vcs_oid_is_zero(commit_oid)) {
        *out_tree = VCS_OID_ZERO;
        return 1;
    }

    if (!vcsobjstore_read(store, commit_oid, &type, &data, &len)) return 0;
    vcs_commit_init(&c);
    if (type != VCS_OBJ_COMMIT || !vcs_commit_decode(data, len, &c)) { free(data); return 0; }
    free(data);
    *out_tree = c.tree;
    vcs_commit_free(&c);
    return 1;
}

static void join_rel(char *out, size_t sz, const char *prefix, const char *name)
{
    if (prefix[0] == '\0') {
        strncpy(out, name, sz - 1);
        out[sz - 1] = '\0';
    } else {
        snprintf(out, sz, "%s/%s", prefix, name);
    }
}

int vcs_flatten_committed_tree(const VcsObjStore *store, const VcsOid *tree_oid,
                               const char *rel_prefix, VcsFlatList *out)
{
    VcsObjType type;
    void *data;
    long len;
    VcsTree tree;
    int i, ok = 1;

    if (vcs_oid_is_zero(tree_oid)) return 1; /* empty tree: nothing to add */

    if (!vcsobjstore_read(store, tree_oid, &type, &data, &len)) return 0;
    if (type != VCS_OBJ_TREE || !vcs_tree_decode(data, len, &tree)) { free(data); return 0; }
    free(data);

    for (i = 0; i < tree.n; i++) {
        char rel_path[1024];
        join_rel(rel_path, sizeof(rel_path), rel_prefix, tree.items[i].name);

        if (tree.items[i].is_dir) {
            if (!vcs_flatten_committed_tree(store, &tree.items[i].oid, rel_path, out)) { ok = 0; break; }
        } else {
            vcs_flat_append(out, rel_path, &tree.items[i].oid);
        }
    }
    vcs_tree_free(&tree);
    return ok;
}
