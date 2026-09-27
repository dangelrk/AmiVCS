/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_REFS_H
#define CLOUDVCS_REFS_H

/*
 * refs.h -- branches (docs/design/08_VCS_Grundkonzept.md: "Ein
 * Branch verweist lediglich auf einen Commit") and HEAD.
 *
 * All branches live in ONE file, <amivcs_dir>/packed-refs -- deliberately
 * not one file per branch (that's Git's loose-refs layout): a handful of
 * branches as tiny separate files would waste a whole filesystem block
 * each on FFS for a few bytes of content (plan section 3). Plain text,
 * one line per ref ("<32-hex-oid> <name>\n") -- no JSON here, so refs.c
 * doesn't need parson just to read two columns.
 *
 * HEAD is its own small file: "ref: refs/heads/<branch>\n" when attached
 * to a branch (the normal case), or a bare 32-hex OID when detached
 * (checked out at a specific commit, not a branch tip).
 */

#include "object.h"

#define VCS_HEAD_FILE        "HEAD"
#define VCS_PACKED_REFS_FILE "packed-refs"
#define VCS_REF_PREFIX       "refs/heads/"

typedef struct {
    char   name[128]; /* full ref name, e.g. "refs/heads/main" */
    VcsOid oid;
} VcsRef;

typedef struct {
    VcsRef *items;
    int n, cap;
} VcsRefList;

/* Loads <amivcs_dir>/packed-refs into a fresh *out. A missing file is not
 * an error -- it means no branches exist yet (mid-init) -- and yields an
 * empty list. Returns 1 on success, 0 only on unexpected allocation
 * failure. */
int vcs_refs_load(const char *amivcs_dir, VcsRefList *out);

/* Serializes *refs to <amivcs_dir>/packed-refs, writing a .tmp file first
 * and renaming it over the target (same atomicity discipline as
 * libcloudsync's syncstate_save() -- an interrupted write must never
 * corrupt the ref list). Returns 1 on success. */
int vcs_refs_save(const char *amivcs_dir, const VcsRefList *refs);

VcsRef *vcs_refs_find(VcsRefList *refs, const char *name);
/* Inserts or overwrites the ref named name. */
void    vcs_refs_upsert(VcsRefList *refs, const char *name, const VcsOid *oid);
void    vcs_refs_remove(VcsRefList *refs, const char *name);
void    vcs_refs_free(VcsRefList *refs);

/* ------------------------------------------------------------------ */

typedef struct {
    int    detached;
    char   branch[112]; /* short name (no "refs/heads/" prefix); valid iff !detached */
    VcsOid oid;          /* valid iff detached */
} VcsHead;

/* Loads <amivcs_dir>/HEAD. Returns 1 on success, 0 if HEAD is missing or
 * malformed (a repo in a consistent state always has one -- vcs_repo_init()
 * writes it). */
int vcs_head_load(const char *amivcs_dir, VcsHead *out);

/* Points HEAD at a branch (attaches it) -- does not create the branch
 * itself if it doesn't exist yet (callers create the ref first). Returns
 * 1 on success. */
int vcs_head_set_branch(const char *amivcs_dir, const char *branch);

/* Points HEAD directly at a commit, detached from any branch. Returns 1
 * on success. */
int vcs_head_set_detached(const char *amivcs_dir, const VcsOid *oid);

/* Resolves HEAD to a concrete commit OID: follows the branch ref when
 * attached, or returns the stored OID directly when detached. *out_oid is
 * VCS_OID_ZERO if HEAD points at a branch that has no commits yet (a
 * freshly-init'd repo, before the first commit) -- that is a valid state,
 * not an error. Returns 1 if HEAD itself could be read, 0 on I/O error. */
int vcs_head_resolve(const char *amivcs_dir, VcsOid *out_oid);

#endif /* CLOUDVCS_REFS_H */
