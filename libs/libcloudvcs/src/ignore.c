/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * ignore.c -- see ignore.h. Pulled out of repo.c so index.c's
 * add_dir_recursive() can share it (previously repo.c-private, hence
 * index.c's "acceptable v1 gap" comment -- now resolved). Adds root-
 * anchored, multi-segment path patterns (git's own gitignore semantics)
 * on top of the original v1 single-segment-per-level matching, which
 * stays byte-for-byte behaviourally unchanged for existing
 * .amivcsignore files.
 */
#include "ignore.h"
#include "objstore.h" /* VCS_DIR_NAME */
#include "localfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VCS_IGNORE_FILE ".amivcsignore"

static VcsIgnoreEntry *ignore_grow(VcsIgnore *ig)
{
    VcsIgnoreEntry *tmp = (VcsIgnoreEntry *)realloc(ig->entries, sizeof(VcsIgnoreEntry) * (size_t)(ig->n + 1));
    if (!tmp) return NULL;
    ig->entries = tmp;
    memset(&ig->entries[ig->n], 0, sizeof(VcsIgnoreEntry));
    return &ig->entries[ig->n];
}

static void entry_free(VcsIgnoreEntry *e)
{
    int i;
    if (e->is_path) {
        for (i = 0; i < e->nsegs; i++) pattern_free(e->segs[i]);
        free(e->segs);
        free(e->doubleglob);
    } else {
        pattern_free(e->seg);
    }
}

/* Splits line (already stripped of any dir_only trailing slash and any
 * leading anchor slash) into path segments, compiling each ("**" left
 * uncompiled -- nothing to glob-match). Empty segments (from a stray
 * "//") are dropped. Returns 1 on success (even zero segments), 0 on
 * allocation failure. */
static int compile_path_segments(const char *line, VcsIgnoreEntry *e)
{
    char buf[256];
    char *p, *start;
    size_t len = strlen(line);

    if (len >= sizeof(buf)) return 0;
    memcpy(buf, line, len + 1);

    e->segs = NULL;
    e->doubleglob = NULL;
    e->nsegs = 0;

    p = buf;
    while (*p) {
        start = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }

        if (start[0] == '\0') continue; /* collapse doubled slashes */

        {
            Pattern **nsegs = (Pattern **)realloc(e->segs, sizeof(Pattern *) * (size_t)(e->nsegs + 1));
            int      *ndg   = (int *)realloc(e->doubleglob, sizeof(int) * (size_t)(e->nsegs + 1));
            if (!nsegs || !ndg) { free(nsegs); free(ndg); return 0; }
            e->segs = nsegs;
            e->doubleglob = ndg;
        }

        if (strcmp(start, "**") == 0) {
            e->segs[e->nsegs] = NULL;
            e->doubleglob[e->nsegs] = 1;
        } else {
            Pattern *pat = pattern_compile(start);
            if (!pat) return 0;
            e->segs[e->nsegs] = pat;
            e->doubleglob[e->nsegs] = 0;
        }
        e->nsegs++;
    }
    return 1;
}

void vcs_ignore_load(const char *root, VcsIgnore *ig)
{
    char path[1024];
    FILE *fp;
    char line[256];

    ig->entries = NULL;
    ig->n = 0;

    localfs_join(path, sizeof(path), root, VCS_IGNORE_FILE);
    fp = fopen(path, "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        int dir_only;
        int is_path;
        const char *pat_start;
        VcsIgnoreEntry *e;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        /* '#' starts a comment UNLESS it's AmigaDOS's own "#?" wildcard
         * marker (pattern.h) -- without this exception, "#?.o"/"#?.lib"
         * (both in DEFAULT_IGNORE) would silently never match anything,
         * a latent bug inherited from v1's original comment check. */
        if (len == 0 || (line[0] == '#' && line[1] != '?')) continue;

        dir_only = (len > 0 && line[len - 1] == '/');
        if (dir_only) line[--len] = '\0';
        if (len == 0) continue;

        pat_start = line;
        is_path = (strchr(line, '/') != NULL);
        if (is_path && line[0] == '/') pat_start = line + 1; /* leading slash is just the anchor */
        if (pat_start[0] == '\0') continue;

        e = ignore_grow(ig);
        if (!e) continue;

        e->dir_only = dir_only;
        e->is_path = is_path;

        if (is_path) {
            if (!compile_path_segments(pat_start, e)) { entry_free(e); continue; }
        } else {
            e->seg = pattern_compile(pat_start);
            if (!e->seg) continue; /* leave slot zeroed; matches nothing */
        }
        ig->n++;
    }
    fclose(fp);
}

/* Backtracking segment-by-segment match, "**" consuming zero or more
 * path segments -- the same idea as glob_match()'s own "*" handling, one
 * level up (path segments instead of characters). */
static int segs_match(Pattern *const *pat, const int *doubleglob, int npat,
                      const char *const *path, int npath)
{
    if (npat == 0) return npath == 0;

    if (doubleglob[0]) {
        int k;
        for (k = 0; k <= npath; k++) {
            if (segs_match(pat + 1, doubleglob + 1, npat - 1, path + k, npath - k)) return 1;
        }
        return 0;
    }

    if (npath == 0) return 0;
    if (!pattern_match(pat[0], path[0])) return 0;
    return segs_match(pat + 1, doubleglob + 1, npat - 1, path + 1, npath - 1);
}

static int path_pattern_match(const VcsIgnoreEntry *e, const char *relpath)
{
    char buf[1024];
    const char *segs[128];
    int nsegs = 0;
    char *p, *start;
    size_t len = strlen(relpath);

    if (len >= sizeof(buf)) return 0;
    memcpy(buf, relpath, len + 1);

    p = buf;
    while (*p) {
        if (nsegs >= 128) return 0;
        start = p;
        while (*p && *p != '/') p++;
        if (*p == '/') { *p = '\0'; p++; }
        segs[nsegs++] = start;
    }

    return segs_match(e->segs, e->doubleglob, e->nsegs, segs, nsegs);
}

int vcs_ignore_match(const VcsIgnore *ig, const char *relpath, int is_dir)
{
    int i;
    const char *slash = strrchr(relpath, '/');
    const char *base = slash ? slash + 1 : relpath;

    if (strcmp(base, VCS_DIR_NAME) == 0) return 1;

    for (i = 0; i < ig->n; i++) {
        const VcsIgnoreEntry *e = &ig->entries[i];
        if (e->dir_only && !is_dir) continue;

        if (!e->is_path) {
            if (e->seg && pattern_match(e->seg, base)) return 1;
        } else {
            if (path_pattern_match(e, relpath)) return 1;
        }
    }
    return 0;
}

void vcs_ignore_free(VcsIgnore *ig)
{
    int i;
    for (i = 0; i < ig->n; i++) entry_free(&ig->entries[i]);
    free(ig->entries);
    ig->entries = NULL;
    ig->n = 0;
}
