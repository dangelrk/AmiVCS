/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_MERGESTATE_H
#define CLOUDVCS_MERGESTATE_H

/*
 * mergestate.h -- the state of a merge that stopped on conflicts (one
 * file format, like index.h). Two files in .amivcs/:
 *
 *   MERGE_HEAD   line 1: theirs commit (hex), line 2: ours commit (hex),
 *                line 3: label shown to the user ("feature", "origin/main")
 *   MERGE_FILES  one line per path: <status>\t<kind>\t<flags>\t<path>
 *                status  U unresolved, R resolved, - only touched
 *                kind    content | binary | add-add | modify-delete |
 *                        delete-modify | touched
 *                flags   "t" if the merge wrote <path>.theirs, else "-"
 *                path    repository path (UTF-8, vcsname.h)
 *
 * MERGE_HEAD's presence is what "a merge is in progress" means. It is
 * written last on save and removed first on clear, so a half-written
 * state never looks active. Every path the merge writes or deletes is
 * listed ("touched" or a conflict kind) -- `merge --abort` restores
 * exactly those and never touches a file the user owns.
 */

#include "object.h"

#define VCS_MERGE_HEAD_FILE  "MERGE_HEAD"
#define VCS_MERGE_FILES_FILE "MERGE_FILES"

typedef struct {
    char  status;     /* 'U', 'R' or '-' */
    char  kind[16];
    int   theirs_file;/* 1: <path>.theirs was written by the merge */
    char *path;       /* malloc'd */
} VcsMergeFile;

typedef struct {
    int    active;
    VcsOid theirs;
    VcsOid ours;
    char   label[192];
    VcsMergeFile *items;
    int    n, cap;
} VcsMergeState;

void vcs_mergestate_init(VcsMergeState *st);
void vcs_mergestate_free(VcsMergeState *st);

/* Reads the state. Returns 1 (st->active 0 if no merge is in progress),
 * 0 on a damaged MERGE_HEAD. */
int  vcs_mergestate_load(const char *amivcs_dir, VcsMergeState *st);
/* Writes MERGE_FILES, then MERGE_HEAD (tmp+rename each). Returns 1. */
int  vcs_mergestate_save(const char *amivcs_dir, const VcsMergeState *st);
/* Removes MERGE_HEAD, then MERGE_FILES. */
void vcs_mergestate_clear(const char *amivcs_dir);
/* 1 if a merge is in progress. */
int  vcs_mergestate_active(const char *amivcs_dir);

/* Adds path, or updates it if already listed (a conflict kind replaces
 * "touched"). Returns the entry, NULL when out of memory. */
VcsMergeFile *vcs_mergestate_put(VcsMergeState *st, char status, const char *kind,
                                 int theirs_file, const char *path);
VcsMergeFile *vcs_mergestate_find(VcsMergeState *st, const char *path);
int  vcs_mergestate_unresolved(const VcsMergeState *st);

/* Marks every unresolved path equal to relpath or below it (a
 * directory) resolved and saves -- `add` during a merge. relpath "" means
 * all of them. A no-op without a merge in progress. Returns 1. */
int  vcs_mergestate_mark_resolved(const char *amivcs_dir, const char *relpath);

/* 1 if kind is one of the conflict kinds (not "touched"). */
int  vcs_mergestate_is_conflict(const VcsMergeFile *f);

#endif /* CLOUDVCS_MERGESTATE_H */
