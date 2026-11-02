/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_INDEX_H
#define CLOUDVCS_INDEX_H

/*
 * index.h -- file-level staging area (git-gui-plan v2 section 1, a
 * deliberate reversal of ADR 09's original "kein Staging/Index"
 * decision). No new object format: the index is simply the OID of an
 * ordinary VCS_OBJ_TREE object (object.h), persisted as one 32-hex-char
 * line in <amivcs_dir>/index -- same shape as HEAD's detached form
 * (refs.h). A freshly-init'd repo (or one that predates this feature and
 * has no index file yet) behaves as if its index mirrors HEAD's tree
 * exactly, computed on demand by vcs_index_load() rather than written
 * eagerly -- existing repositories keep working without a migration
 * step.
 *
 * repo.c owns vcs_repo_status_raw()/vcs_repo_commit(): they call into
 * this module rather than the other way around, matching the module-
 * encapsulation rule (repo.c = orchestration, index.c = one file format).
 */

#include "object.h"
#include "repo.h"

#define VCS_INDEX_FILE "index"

/* Resolves the current index tree OID:
 *   - reads <amivcs_dir>/index if it exists;
 *   - otherwise falls back to HEAD's committed tree (VCS_OID_ZERO if HEAD
 *     has no commits yet) WITHOUT writing the file -- the index only
 *     becomes an on-disk fact once something actually stages a change.
 * Returns 1 on success, 0 on I/O error. */
int vcs_index_load(const char *amivcs_dir, VcsOid *out_tree_oid);

/* Writes tree_oid as the current index. Same tmp+rename atomicity as
 * vcs_refs_save(). Returns 1 on success. */
int vcs_index_save(const char *amivcs_dir, const VcsOid *tree_oid);

/* Alias for vcs_index_save(), named for the call sites that mean it
 * ("the index now mirrors this newly-materialized tree exactly") --
 * called after a successful checkout/switch/merge so the index never
 * carries stale staged state across a branch change. */
int vcs_index_reset_to(const char *amivcs_dir, const VcsOid *tree_oid);

/* Stages relpath (a file, or a directory -- staged recursively) for the
 * next commit: hashes/writes the current working-tree content as
 * blob(s), then rebuilds the index tree and every ancestor tree along
 * relpath's path so the root index OID reflects the change, and persists
 * it via vcs_index_save(). Directory recursion is .amivcsignore-aware
 * (ignore.h), same as status()/commit() -- an ignored child is skipped,
 * though relpath itself is still staged if named explicitly, ignored or
 * not. A path that no longer exists on disk stages its deletion (also
 * for everything the index has below a deleted directory). Returns 1 on
 * success, 0 if relpath is unknown both on disk and in the index, or on
 * I/O error. */
/* relpath here and below is a repository path (UTF-8, vcsname.h); a
 * name the local codeset can't show needs the caller's name table
 * (vcs_repo_status_raw() or vcs_repo_learn_names() first). */
int vcs_index_add(VcsRepo *repo, const char *relpath);

/* Removes relpath from the index tree ("git rm --cached" -- the working
 * copy is untouched). Returns 1 on success (including if relpath was
 * already absent from the index -- matches the New/Modified/Deleted
 * status labels, "remove" on something not staged is a no-op, not an
 * error), 0 on I/O error. */
int vcs_index_remove(VcsRepo *repo, const char *relpath);

/* Takes relpath (a file, or everything below a directory) back out of
 * the staging area: the index entry returns to HEAD's version, or goes
 * away if HEAD doesn't have the path ("git restore --staged"). The
 * working copy is untouched. Returns 1 on success. */
int vcs_index_unstage(VcsRepo *repo, const char *relpath);

/* Puts blob_oid into the index under relpath without reading the working
 * tree (merge). Returns 1 on success. */
int vcs_index_set_blob(VcsRepo *repo, const char *relpath, const VcsOid *blob_oid);

/* Convenience: stages every path vcs_repo_status_raw() currently reports
 * as unstaged (New/Modified -> add, Deleted -> remove) in one call
 * ("git add -A"). Returns 1 on success. */
int vcs_index_add_all(VcsRepo *repo);

#endif /* CLOUDVCS_INDEX_H */
