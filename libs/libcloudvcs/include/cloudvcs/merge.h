/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_MERGE_H
#define CLOUDVCS_MERGE_H

/*
 * merge.h -- client-side-only merging (docs/adr/09_ADR_VCS_Design.md:
 * "Merge findet ausschliesslich auf dem Client statt. Der Server besitzt
 * keinerlei Merge-Logik."). v1 scope, deliberately: fast-forward, a
 * simple file-level 3-way merge, and conflict detection -- no line-level
 * diff/merge (AMIVCS whole-file-hashes content; a binary Amiga file has
 * no meaningful "line"), no automatic conflict resolution of any kind.
 *
 * The 3-way decision matrix (base/ours/theirs presence + content) is the
 * same shape as libcloudsync's local/remote/baseline sync matrix --
 * deliberately: both are "compare three states of the same path, act
 * unambiguously or flag a conflict, never guess" problems.
 */

#include "repo.h"

/* Merges branch into the current branch:
 *   - refuses if the working tree is dirty (same policy as switch()/
 *     checkout()) or if branch doesn't exist;
 *   - "Already up to date" (returns 1, prints a message) if branch's
 *     commit is already an ancestor of HEAD;
 *   - fast-forwards (moves the current branch's ref, materializes the
 *     new tree, returns 1) if HEAD is an ancestor of branch's commit;
 *   - otherwise performs a file-level 3-way merge against the nearest
 *     common ancestor. If every path resolves unambiguously, writes a
 *     merge commit (two parents) and advances the current branch,
 *     returning 1. If any path conflicts (changed differently on both
 *     sides), the merge does NOT complete: no commit is created, the
 *     conflicting file keeps "ours" content, a sibling "<path>.theirs"
 *     is written for manual inspection, and this returns 0 -- resolve by
 *     hand, then commit normally.
 */
int vcs_repo_merge(VcsRepo *repo, const char *branch);

/* Core of vcs_repo_merge(): same fast-forward/3-way/conflict logic, but
 * against an arbitrary commit oid rather than a local branch's tip --
 * used directly by remote.c's pull, where "theirs" is a commit just
 * fetched from the server and not (yet, or ever) kept under a local
 * refs/heads/ branch. theirs_label is display-only (error messages, the
 * merge commit's message) -- pass whatever name makes sense to the user
 * (e.g. "origin/main"). */
int vcs_repo_merge_commit(VcsRepo *repo, const VcsOid *theirs_commit, const char *theirs_label);

#endif /* CLOUDVCS_MERGE_H */
