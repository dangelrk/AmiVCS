/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_MERGE_H
#define CLOUDVCS_MERGE_H

/*
 * merge.h -- client-side-only merging (docs/adr/09_ADR_VCS_Design.md:
 * "Merge findet ausschliesslich auf dem Client statt. Der Server besitzt
 * keinerlei Merge-Logik."): fast-forward, or a 3-way merge against the
 * nearest common ancestor. Per path the decision matrix (base/ours/
 * theirs presence + content) is the same shape as libcloudsync's sync
 * matrix -- act unambiguously or flag a conflict, never guess.
 *
 * A text file both sides changed is merged line by line (merge3.h,
 * addendum 2026-11 to ADR 09); what still collides gets git's conflict
 * markers in the file. Binary or very large files are merged as a whole:
 * the file keeps ours and theirs is written beside it as <path>.theirs.
 * Modified on one side and deleted on the other: the modified version
 * stays in the working tree.
 *
 * A merge that stops on conflicts leaves a merge state (mergestate.h).
 * While it exists, commit refuses until every conflict is resolved
 * (`add` the file, or vcs_repo_merge_resolve()) and then records both
 * parents; merge/pull/switch/checkout refuse; vcs_repo_merge_abort()
 * puts everything back.
 */

#include "repo.h"

/* Merges branch into the current branch:
 *   - refuses if the working tree is dirty (same policy as switch()/
 *     checkout()) or if branch doesn't exist;
 *   - "Already up to date" (returns 1, prints a message) if branch's
 *     commit is already an ancestor of HEAD;
 *   - fast-forwards (moves the current branch's ref, materializes the
 *     new tree, returns 1) if HEAD is an ancestor of branch's commit;
 *   - otherwise performs a 3-way merge against the nearest
 *     common ancestor. If every path resolves unambiguously, writes a
 *     merge commit (two parents) and advances the current branch,
 *     returning 1. If any path conflicts (changed differently on both
 *     sides) and the line merge can't settle it, the merge does NOT
 *     complete: no commit is created, the merge state is saved, the
 *     conflicts are written as described above, and this returns 0 --
 *     resolve, `add`, then commit (it becomes the merge commit).
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

/* Ends a merge in progress without committing: every path the merge
 * wrote or deleted gets ours' version back (or is removed if ours didn't
 * have it), the .theirs files go, the index returns to ours' tree and
 * the merge state is removed. Files not under version control are left
 * alone. Returns 1, 0 if no merge is in progress or on I/O error. */
int vcs_repo_merge_abort(VcsRepo *repo);

/* Resolves one conflicting path of the merge in progress by taking one
 * side's version as a whole (take_theirs 0: ours, 1: theirs; a side that
 * doesn't have the file deletes it), staging it and marking the path
 * resolved. path is a repository path. Returns 1 on success. */
int vcs_repo_merge_resolve(VcsRepo *repo, const char *path, int take_theirs);

#endif /* CLOUDVCS_MERGE_H */
