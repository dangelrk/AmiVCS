/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_IGNORE_H
#define CLOUDVCS_IGNORE_H

/*
 * ignore.h -- .amivcsignore matching, shared by repo.c's three working-
 * tree walkers (snapshot/status/materialize) AND index.c's
 * add_dir_recursive() (originally repo.c-private; pulled out so index.c
 * can reuse it -- see index.h's file comment on the module-encapsulation
 * rule).
 *
 * Two kinds of pattern line, distinguished by whether they contain a
 * slash:
 *
 *   - Segment pattern (no slash, e.g. "#?.o", "bin/"): matches just the
 *     basename, applied at every directory level, exactly like v1. A
 *     trailing slash means "only matches a directory" (dir_only).
 *   - Path pattern (contains a slash, e.g. "/build", "docs/logname",
 *     "double-star/tmp/"): anchored at the repo root and matched against
 *     the FULL relative path, segment-by-segment, so a single wildcard
 *     never crosses a slash (git's own gitignore semantics). A leading
 *     slash is just the anchor and is consumed, not part of the first
 *     segment. A lone "**" segment matches zero or more path segments.
 *
 * The underlying glob engine (pattern.h's pattern_compile()/
 * pattern_match(), one wildcard segment at a time) is untouched -- this
 * module only adds the path-splitting layer on top of it.
 */

#include "pattern.h"

/* One compiled pattern line -- either a single-segment Pattern (the v1
 * shape) or an array of per-segment Patterns anchored at the root. A
 * segment whose source text was literally "**" has a NULL Pattern* in
 * segs (nothing to glob-match) and doubleglob[i] = 1. */
typedef struct {
    int       is_path;   /* 1: path pattern (segs/nsegs valid), 0: segment (seg valid) */
    int       dir_only;
    Pattern  *seg;        /* valid when !is_path */
    Pattern **segs;       /* one per path segment, valid when is_path */
    int      *doubleglob; /* parallel to segs: 1 if that segment is "**" */
    int       nsegs;
} VcsIgnoreEntry;

typedef struct {
    VcsIgnoreEntry *entries;
    int             n;
} VcsIgnore;

/* Loads <root>/.amivcsignore into a fresh *ig (zeroed/empty if the file
 * doesn't exist or is empty -- not an error, just "nothing ignored"). */
void vcs_ignore_load(const char *root, VcsIgnore *ig);

/* Does relpath (repo-root-relative, slash-separated, no leading/trailing
 * slash) match any pattern in ig? is_dir must reflect whether relpath
 * names a directory (dir_only patterns need it). VCS_DIR_NAME (".amivcs")
 * always matches, regardless of ig's contents. */
int vcs_ignore_match(const VcsIgnore *ig, const char *relpath, int is_dir);

void vcs_ignore_free(VcsIgnore *ig);

#endif /* CLOUDVCS_IGNORE_H */
