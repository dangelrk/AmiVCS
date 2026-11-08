/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_REPODIFF_H
#define CLOUDVCS_REPODIFF_H

/*
 * repodiff.h -- what changed between two states of a repository, file by
 * file and line by line (diff.h): `diff`, `show` and the GUIs' diff view.
 * A state is a commit, the index (staged) or the working tree.
 */

#include "repo.h"
#include "diff.h"

typedef enum {
    VCS_SIDE_COMMIT,   /* side.oid: a commit (VCS_OID_ZERO = the empty tree) */
    VCS_SIDE_INDEX,
    VCS_SIDE_WORKDIR
} VcsSideKind;

typedef struct {
    VcsSideKind kind;
    VcsOid      oid;
} VcsDiffSide;

/* One changed file. old_oid/new_oid are VCS_OID_ZERO on the side that
 * doesn't have the file; a working-tree file's oid is its content hash. */
typedef struct {
    char        kind;     /* 'N' new, 'M' modified, 'D' deleted */
    const char *path;     /* repository path (UTF-8) */
    VcsOid      old_oid;
    VcsOid      new_oid;
    int         binary;   /* 1: binary or larger than VCS_DIFF_MAX_BYTES -- no hunks follow */
} VcsFileDiff;

typedef struct {
    /* called once per changed file, before its hunks; may be NULL */
    void (*file)(const VcsFileDiff *f, void *ctx);
    VcsDiffSink lines;    /* hunks and lines (diff.h); members may be NULL */
} VcsRepoDiffSink;

/* Diffs `from` against `to`. paths (npaths of them, repository paths; a
 * directory covers everything below it) limits the output, NULL/0 = all.
 * Files new in the working tree but not in the index (not added yet) are
 * only shown if a path names them. Returns the number of changed files,
 * -1 on I/O error. */
int vcs_repo_diff(VcsRepo *repo, const VcsDiffSide *from, const VcsDiffSide *to,
                  const char *const *paths, int npaths,
                  const VcsRepoDiffSink *sink, void *ctx);

/* Resolves what the user calls a commit: "HEAD", a branch name
 * (repository form), a full commit id or a unique prefix of at least 4
 * hex digits, each optionally followed by "^" or "~N" (first parent, N
 * times). Returns 1 with *out set, 0 after a UI_ERROR. */
int vcs_repo_resolve_rev(VcsRepo *repo, const char *rev, VcsOid *out);
/* Same check without a message: 1 if rev names a commit. */
int vcs_repo_rev_exists(VcsRepo *repo, const char *rev);

#endif /* CLOUDVCS_REPODIFF_H */
