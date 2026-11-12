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
#include "repo.h"

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

/* Name table for one operation (vcsname.h): fresh, .amivcs/localnames,
 * then the paths of a and b (either may be NULL; they override). */
void vcs_names_begin_flat(VcsRepo *repo, const VcsFlatList *a, const VcsFlatList *b);

/* 1 if no two paths of a (and b, may be NULL) would be the same local
 * file; else 0 after a UI_ERROR naming both. Learn them first. */
int vcs_names_unique_flat(const VcsFlatList *a, const VcsFlatList *b);

/* 1 if every name of f fits the file system under root in its local
 * form (vcs_names_fit()); else 0 after naming the first long one. Called
 * before anything is written: classic FFS keeps 30 characters and would
 * silently merge two long names into one file. */
int vcs_names_fit_flat(const char *root, const VcsFlatList *f);

/* 1 if no file outside `from` (not under version control) sits where a
 * path of `to` that `from` lacks would be written (a different file, or
 * a file in place of a needed directory); else 0 after a UI_ERROR. */
int vcs_names_in_the_way(VcsRepo *repo, const VcsFlatList *from, const VcsFlatList *to);

/* Removes the "theirs" copy a conflicted merge wrote for repo_path
 * (vcs_theirs_rel() with the working tree's name length). */
void vcs_remove_theirs(VcsRepo *repo, const char *repo_path);

/* Two-way scan of two sorted flat lists: cb gets ('N'ew / 'M'odified /
 * 'D'eleted, path) for each difference of `work` against `head`; cb may
 * be NULL to just count. Returns the number of differences. */
int vcs_flat_diff(const VcsFlatList *head, const VcsFlatList *work,
                  void (*cb)(char kind, const char *path, void *ctx), void *ctx);

/* The working tree as a sorted flat list (path -> content hash), with
 * .amivcsignore applied -- what status compares against. Returns 1. */
int vcs_flatten_workdir_list(VcsRepo *repo, VcsFlatList *out);

#endif /* CLOUDVCS_TREEUTIL_H */
