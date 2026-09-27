/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_REMOTE_H
#define CLOUDVCS_REMOTE_H

/*
 * remote.h -- clone/push/pull against a standalone AMIVCS server
 * (server/, see its api.php) via libcloudhttp's generic transport
 * (http.h's http_request()/http_request_hdr() -- the AmiCloud-specific
 * cmd.c is NOT used here; AMIVCS has its own server contract, kept
 * entirely in this file, per the module-encapsulation rule).
 *
 * The server is "dumb" (ADR 09: no merge logic at all) -- it only stores
 * objects and refs and enforces ref updates via compare-and-swap. All
 * negotiation (which objects are missing, fast-forward vs. 3-way vs.
 * conflict) happens here, client-side, reusing repo.h/merge.h exactly
 * like a purely local operation would.
 *
 * AcConfig (core/libcloudcore/include/cloudcore/amicloud.h) is reused
 * unchanged for the remote connection (server/username/password/
 * verify_ssl) -- see plan section 5, no core extension needed.
 */

#include "repo.h"
#include "amicloud.h"

/* Creates repo_name on the server. Returns 1 on success, 0 on error
 * (including "already exists" -- callers that treat that as fine, e.g. a
 * repeat clone, should check first via a distinct code path if they care;
 * v1's callers just report failure and stop). */
int vcs_remote_create_repo(const AcConfig *cfg, const char *repo_name);

/* Clones repo_name from the server into local_dir (created if missing):
 * initializes a new local repository there, fetches every object
 * reachable from every branch the server reports, mirrors all of them as
 * local refs/heads/ branches, and checks out "main" if the server has
 * it (else the first branch in whatever order the server returned).
 * Remembers repo_name in <local_dir>/.amivcs/config so push/pull need no
 * repeated argument. Returns 1 on success. */
int vcs_remote_clone(const AcConfig *cfg, const char *repo_name, const char *local_dir);

/* Pushes the current branch: uploads every object reachable from its tip
 * that the server doesn't already have (action=has negotiation), then
 * updates the branch's ref there via compare-and-swap against the
 * server's current value. Refuses (0) with a clear message if the server
 * rejects it as a non-fast-forward (409) -- pull and merge first, the
 * server never merges (ADR 09). repo_name may be NULL to reuse
 * .amivcs/config's remembered remote; if given, it overrides and
 * re-remembers it, creating the repo on the server first if needed. */
int vcs_remote_push(VcsRepo *repo, const AcConfig *cfg, const char *repo_name);

/* Fetches the named remote branch (or the current local branch's name if
 * branch is NULL) and merges it into the current branch via the exact
 * same logic as `amivcs merge` (merge.h's vcs_repo_merge_commit) --
 * fast-forward, clean 3-way, or a refused conflict, identically.
 * repo_name: see vcs_remote_push(). */
int vcs_remote_pull(VcsRepo *repo, const AcConfig *cfg, const char *repo_name, const char *branch);

#endif /* CLOUDVCS_REMOTE_H */
