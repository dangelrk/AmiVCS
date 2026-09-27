/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_OBJECT_H
#define CLOUDVCS_OBJECT_H

/*
 * object.h — AMIVCS's object model: Blob, Tree, Commit (docs/design/
 * 08_VCS_Grundkonzept.md). Deliberately NOT a Git clone (docs/adr/
 * 09_ADR_VCS_Design.md): objects are a compact binary encoding, not
 * zlib-deflated "type size\0content" -- no compression in v1 (little CPU),
 * no floating point anywhere (timestamps are plain integers so a 68000
 * without an FPU never needs mathieeedoubbas just to open a commit).
 *
 * OID = plain MD5 of an object's own encoded payload (blobs: the raw file
 * bytes; trees/commits: their own binary encoding below) -- there is no
 * type byte mixed into the hash, unlike Git. A small single-user Amiga VCS
 * doesn't need Git's collision-domain separation between types.
 *
 * Every encoded payload starts with a one-byte format version
 * (VCS_FORMAT_VERSION) so a future encoding change (e.g. a longer OID if
 * MD5 is ever swapped out) has somewhere to hang a repo-format bump
 * without breaking readers of existing objects.
 */

#include <stddef.h>

#define VCS_OID_SIZE        16  /* raw MD5 bytes */
#define VCS_OID_HEX         33  /* 32 hex chars + NUL, matches md5_to_hex() */
#define VCS_FORMAT_VERSION   1

typedef enum { VCS_OBJ_BLOB = 0, VCS_OBJ_TREE = 1, VCS_OBJ_COMMIT = 2 } VcsObjType;

typedef struct { unsigned char b[VCS_OID_SIZE]; } VcsOid;

extern const VcsOid VCS_OID_ZERO;

int  vcs_oid_equal(const VcsOid *a, const VcsOid *b);
int  vcs_oid_is_zero(const VcsOid *o);
void vcs_oid_to_hex(const VcsOid *o, char hex[VCS_OID_HEX]);
/* Accepts exactly 32 hex chars (case-insensitive). Returns 1 on success, 0
 * if hex is malformed -- caller's *out is left untouched on failure. */
int  vcs_oid_from_hex(const char *hex, VcsOid *out);

/* ------------------------------------------------------------------ */
/* Tree -- one directory level: sorted (name, type, oid) entries.       */

typedef struct {
    char   name[256]; /* one path segment, no separators -- like AcEntry.name */
    int    is_dir;
    VcsOid oid;        /* blob oid (is_dir=0) or tree oid (is_dir=1) */
} VcsTreeEntry;

typedef struct {
    VcsTreeEntry *items;
    int n, cap;
} VcsTree;

void vcs_tree_init(VcsTree *t);
void vcs_tree_free(VcsTree *t);

/* Inserts a new entry or overwrites the existing one with the same name,
 * keeping items sorted by name (byte-wise) -- vcs_tree_encode() relies on
 * this order, so callers must never bypass it and touch items[] directly. */
void vcs_tree_upsert(VcsTree *t, const char *name, int is_dir, const VcsOid *oid);

/* Binary search by name (items[] is kept sorted by vcs_tree_upsert()).
 * NULL if not present. */
const VcsTreeEntry *vcs_tree_find(const VcsTree *t, const char *name);

/* Serializes *t. *out_data is malloc'd; caller frees. Returns 1 on success. */
int vcs_tree_encode(const VcsTree *t, void **out_data, long *out_len);

/* Parses data/len into *t. t must be vcs_tree_init()'d (fresh or freed)
 * before calling. Returns 1 on success, 0 on a malformed/truncated buffer. */
int vcs_tree_decode(const void *data, long len, VcsTree *t);

/* ------------------------------------------------------------------ */
/* Commit -- tree + parent(s) + metadata.                                */

/* At most 2 parents: plenty for a fast-forward or a single 3-way merge
 * (ADR 09 -- no octopus merges, this isn't a Git clone). */
#define VCS_MAX_PARENTS 2

typedef struct {
    VcsOid tree;
    VcsOid parents[VCS_MAX_PARENTS];
    int    nparents; /* 0 (root commit), 1, or 2 (merge commit) */
    long   mtime;    /* UTC epoch seconds */
    char   author[64];
    char  *message;  /* malloc'd, NUL-terminated; owned by the VcsCommit */
} VcsCommit;

void vcs_commit_init(VcsCommit *c);
void vcs_commit_free(VcsCommit *c);

int vcs_commit_encode(const VcsCommit *c, void **out_data, long *out_len);

/* Parses data/len into *c. c must be vcs_commit_init()'d beforehand.
 * Returns 1 on success, 0 on a malformed/truncated buffer. */
int vcs_commit_decode(const void *data, long len, VcsCommit *c);

#endif /* CLOUDVCS_OBJECT_H */
