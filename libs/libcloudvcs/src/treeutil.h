/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_TREEUTIL_H
#define CLOUDVCS_TREEUTIL_H

/*
 * treeutil.h -- private helper shared by repo.c (status/is_dirty) and
 * merge.c (3-way file-level merge): flattens a committed Tree object,
 * recursively expanding subtrees, into a single sorted (path -> blob oid)
 * list. Not part of libcloudvcs's public API (include/cloudvcs/) -- this
 * is internal plumbing for comparing/diffing committed trees.
 */

#include "object.h"
#include "objstore.h"

typedef struct {
    char  *path; /* malloc'd, '/'-joined relative to repo root */
    VcsOid oid;
} VcsFlatEntry;

typedef struct {
    VcsFlatEntry *items;
    int n, cap;
} VcsFlatList;

void vcs_flat_init(VcsFlatList *f);
void vcs_flat_free(VcsFlatList *f);
void vcs_flat_append(VcsFlatList *f, const char *path, const VcsOid *oid);
/* Sorts items[] by path -- required before diffing two lists. */
void vcs_flat_sort(VcsFlatList *f);
/* Binary search by path (only meaningful after vcs_flat_sort()). NULL if
 * not present. */
const VcsFlatEntry *vcs_flat_find(const VcsFlatList *f, const char *path);

/* Reads commit_oid's tree field. commit_oid == VCS_OID_ZERO yields the
 * empty tree (VCS_OID_ZERO) -- a valid state for a branch with no commits
 * yet, not an error. Returns 1 on success, 0 on I/O error or if commit_oid
 * doesn't refer to a VCS_OBJ_COMMIT. */
int vcs_commit_tree(const VcsObjStore *store, const VcsOid *commit_oid, VcsOid *out_tree);

/* Recursively expands tree_oid (a VCS_OBJ_TREE) into out, appending one
 * entry per FILE (directories are only recursed into, never appended
 * themselves -- callers only ever need file-level content identity).
 * tree_oid may be VCS_OID_ZERO (the empty tree -- e.g. a branch with no
 * commits yet), which yields no entries. Returns 1 on success. */
int vcs_flatten_committed_tree(const VcsObjStore *store, const VcsOid *tree_oid,
                               const char *rel_prefix, VcsFlatList *out);

#endif /* CLOUDVCS_TREEUTIL_H */
