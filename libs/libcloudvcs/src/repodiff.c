/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * repodiff.c -- see repodiff.h.
 */
#include "repodiff.h"
#include "index.h"
#include "treeutil.h"
#include "localfs.h"
#include "vcsname.h"
#include "ui_hooks.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the flat (path -> oid) list of one side ---- */
static int side_flat(VcsRepo *repo, const VcsDiffSide *side, VcsFlatList *out)
{
    VcsOid tree;
    vcs_flat_init(out);
    switch (side->kind) {
    case VCS_SIDE_COMMIT:
        if (!vcs_commit_tree(&repo->store, &side->oid, &tree)) return 0;
        break;
    case VCS_SIDE_INDEX:
        if (!vcs_index_load(repo->amivcs_dir, &tree)) return 0;
        break;
    default:
        return vcs_flatten_workdir_list(repo, out);
    }
    if (!vcs_flatten_committed_tree(&repo->store, &tree, "", out)) return 0;
    vcs_flat_sort(out);
    return 1;
}

/* content of one side's version of path, if it is text of a size we
 * diff: 1 = *data (malloc'd), -1 = binary/too big, 0 = error */
static int side_content(VcsRepo *repo, const VcsDiffSide *side, const char *path, const VcsOid *oid,
                        void **data, long *len)
{
    *data = NULL;
    *len = 0;
    if (vcs_oid_is_zero(oid)) return 1;   /* the side doesn't have it: empty */
    if (side->kind == VCS_SIDE_WORKDIR) {
        char abs_path[1024];
        long size, mtime;
        FILE *fp;
        if (!vcs_local_join(abs_path, sizeof(abs_path), repo->root, path)) return 0;
        if (!localfs_stat(abs_path, &size, &mtime)) return 0;
        if (size > VCS_DIFF_MAX_BYTES) return -1;
        *data = malloc((size_t)(size ? size : 1));
        if (!*data) return 0;
        fp = fopen(abs_path, "rb");
        if (!fp) { free(*data); *data = NULL; return 0; }
        *len = (long)fread(*data, 1, (size_t)size, fp);
        fclose(fp);
    } else {
        int r = vcsobjstore_read_blob_max(&repo->store, oid, VCS_DIFF_MAX_BYTES, data, len);
        if (r != 1) return r;
    }
    if (!vcs_diff_is_text(*data, *len)) { free(*data); *data = NULL; return -1; }
    return 1;
}

static int under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    while (n > 0 && dir[n - 1] == '/') n--;
    if (n == 0) return 1;
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

typedef struct {
    VcsRepo *repo;
    const VcsDiffSide *from, *to;
    const VcsFlatList *ff, *tf;
    const VcsFlatList *xf;   /* the index, sorted */
    const char *const *paths;
    int npaths;
    const VcsRepoDiffSink *sink;
    void *ctx;
    int files;
    int error;
} DiffRun;

static void one_change(char kind, const char *path, void *vctx)
{
    DiffRun *r = (DiffRun *)vctx;
    VcsFileDiff fd;
    const VcsFlatEntry *a, *b;
    void *da = NULL, *db = NULL;
    long la = 0, lb = 0;
    int ra, rb, i, named = 0, exact = 0;

    if (r->error) return;
    if (r->npaths > 0) {
        for (i = 0; i < r->npaths && !named; i++) named = under(path, r->paths[i]);
        if (!named) return;
        for (i = 0; i < r->npaths && !exact; i++) exact = strcmp(path, r->paths[i]) == 0;
    }
    /* a file the index doesn't know yet is no change of a tracked file --
     * unless it was named itself (a directory doesn't count) */
    if (kind == 'N' && r->to->kind == VCS_SIDE_WORKDIR && r->from->kind != VCS_SIDE_WORKDIR && !exact
        && !vcs_flat_find(r->xf, path))
        return;

    a = vcs_flat_find(r->ff, path);
    b = vcs_flat_find(r->tf, path);
    fd.kind = kind;
    fd.path = path;
    fd.old_oid = a ? a->oid : VCS_OID_ZERO;
    fd.new_oid = b ? b->oid : VCS_OID_ZERO;

    ra = side_content(r->repo, r->from, path, &fd.old_oid, &da, &la);
    rb = side_content(r->repo, r->to, path, &fd.new_oid, &db, &lb);
    if (ra == 0 || rb == 0) {
        free(da); free(db);
        r->error = 1;
        return;
    }
    fd.binary = ra < 0 || rb < 0;
    r->files++;
    if (r->sink->file) r->sink->file(&fd, r->ctx);
    if (!fd.binary && (r->sink->lines.hunk || r->sink->lines.line)) {
        VcsDiffSink s = r->sink->lines;
        if (vcs_diff_buffers(da ? (const char *)da : "", la, db ? (const char *)db : "", lb,
                             VCS_DIFF_CONTEXT, &s, r->ctx) < 0)
            r->error = 1;
    }
    free(da);
    free(db);
}

int vcs_repo_diff(VcsRepo *repo, const VcsDiffSide *from, const VcsDiffSide *to,
                  const char *const *paths, int npaths,
                  const VcsRepoDiffSink *sink, void *ctx)
{
    VcsFlatList ff, tf, xf;
    VcsOid index_tree;
    DiffRun r;
    int i;

    /* names: both committed sides and the index, before the working tree
     * is read (its local names map back through this table) */
    vcs_flat_init(&ff);
    vcs_flat_init(&tf);
    vcs_flat_init(&xf);
    vcs_names_reset();
    vcs_names_load(repo->amivcs_dir);
    if (vcs_index_load(repo->amivcs_dir, &index_tree)
        && vcs_flatten_committed_tree(&repo->store, &index_tree, "", &xf))
        for (i = 0; i < xf.n; i++) vcs_names_learn(xf.items[i].path);
    vcs_flat_sort(&xf);   /* kept: which new files are staged */
    {
        VcsFlatList hf;
        VcsOid head;
        VcsOid tree;
        vcs_flat_init(&hf);
        if (vcs_head_resolve(repo->amivcs_dir, &head) && vcs_commit_tree(&repo->store, &head, &tree)
            && vcs_flatten_committed_tree(&repo->store, &tree, "", &hf))
            for (i = 0; i < hf.n; i++) vcs_names_learn(hf.items[i].path);
        vcs_flat_free(&hf);
    }
    if (from->kind != VCS_SIDE_WORKDIR && !side_flat(repo, from, &ff)) goto fail;
    if (to->kind != VCS_SIDE_WORKDIR && !side_flat(repo, to, &tf)) goto fail;
    for (i = 0; i < ff.n; i++) vcs_names_learn(ff.items[i].path);
    for (i = 0; i < tf.n; i++) vcs_names_learn(tf.items[i].path);
    if (from->kind == VCS_SIDE_WORKDIR && !side_flat(repo, from, &ff)) goto fail;
    if (to->kind == VCS_SIDE_WORKDIR && !side_flat(repo, to, &tf)) goto fail;

    memset(&r, 0, sizeof(r));
    r.repo = repo;
    r.from = from;
    r.to = to;
    r.ff = &ff;
    r.tf = &tf;
    r.xf = &xf;
    r.paths = paths;
    r.npaths = npaths;
    r.sink = sink;
    r.ctx = ctx;
    vcs_flat_diff(&ff, &tf, one_change, &r);
    vcs_flat_free(&ff);
    vcs_flat_free(&tf);
    vcs_flat_free(&xf);
    return r.error ? -1 : r.files;

fail:
    vcs_flat_free(&ff);
    vcs_flat_free(&tf);
    vcs_flat_free(&xf);
    return -1;
}

/* ---- revisions ---- */

static int first_parent(VcsRepo *repo, const VcsOid *oid, VcsOid *out)
{
    VcsObjType type;
    void *data;
    long len;
    VcsCommit c;
    int ok = 0;
    if (!vcsobjstore_read(&repo->store, oid, &type, &data, &len)) return 0;
    vcs_commit_init(&c);
    if (type == VCS_OBJ_COMMIT && vcs_commit_decode(data, len, &c) && c.nparents > 0) {
        *out = c.parents[0];
        ok = 1;
    }
    free(data);
    vcs_commit_free(&c);
    return ok;
}

static int is_hex(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    return n > 0;
}

static int g_rev_quiet = 0;

int vcs_repo_rev_exists(VcsRepo *repo, const char *rev)
{
    VcsOid oid;
    int ok;
    g_rev_quiet = 1;
    ok = vcs_repo_resolve_rev(repo, rev, &oid);
    g_rev_quiet = 0;
    return ok;
}

int vcs_repo_resolve_rev(VcsRepo *repo, const char *rev, VcsOid *out)
{
    char base[256];
    const char *suffix;
    size_t n;
    int steps = 0, found = 0;

    /* split off ^ / ~N */
    suffix = rev + strcspn(rev, "^~");
    n = (size_t)(suffix - rev);
    if (n == 0 || n >= sizeof(base)) { if (!g_rev_quiet) ui_notify(UI_ERROR, "amivcs: unknown revision '%s'", vcs_shown(rev)); return 0; }
    memcpy(base, rev, n);
    base[n] = '\0';
    while (*suffix) {
        if (*suffix == '^') { steps++; suffix++; }
        else if (*suffix == '~') {
            char *end;
            long k = strtol(suffix + 1, &end, 10);
            if (end == suffix + 1) k = 1;
            if (k < 0) k = 0;
            steps += (int)k;
            suffix = end;
        } else {
            if (!g_rev_quiet) ui_notify(UI_ERROR, "amivcs: unknown revision '%s'", vcs_shown(rev));
            return 0;
        }
    }

    if (strcmp(base, "HEAD") == 0) {
        if (!vcs_head_resolve(repo->amivcs_dir, out) || vcs_oid_is_zero(out)) {
            if (!g_rev_quiet) ui_notify(UI_ERROR, "amivcs: no commits yet");
            return 0;
        }
        found = 1;
    }
    if (!found) {
        VcsRefList refs;
        char fullname[300];
        snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, base);
        if (vcs_refs_load(repo->amivcs_dir, &refs)) {
            VcsRef *r = vcs_refs_find(&refs, fullname);
            if (r) { *out = r->oid; found = 1; }
            vcs_refs_free(&refs);
        }
    }
    if (!found && n == VCS_OID_HEX - 1 && is_hex(base, n) && vcs_oid_from_hex(base, out)
        && vcsobjstore_exists(&repo->store, out))
        found = 1;
    if (!found && n >= 4 && is_hex(base, n)) {
        VcsLogList log;
        int i, hits = 0;
        size_t k;
        for (k = 0; k < n; k++) base[k] = (char)tolower((unsigned char)base[k]);   /* ids print lowercase */
        if (vcs_repo_log_all_raw(repo, &log)) {
            for (i = 0; i < log.n; i++) {
                char hex[VCS_OID_HEX];
                vcs_oid_to_hex(&log.items[i].oid, hex);
                if (strncmp(hex, base, n) == 0) {
                    if (hits == 0) *out = log.items[i].oid;
                    hits++;
                }
            }
            vcs_loglist_free(&log);
        }
        if (hits > 1) {
            if (!g_rev_quiet) ui_notify(UI_ERROR, "amivcs: '%s' is ambiguous -- give more digits", vcs_shown(base));
            return 0;
        }
        found = hits == 1;
    }
    if (!found) {
        if (!g_rev_quiet) ui_notify(UI_ERROR, "amivcs: unknown revision '%s'", vcs_shown(rev));
        return 0;
    }
    while (steps-- > 0) {
        if (!first_parent(repo, out, out)) {
            if (!g_rev_quiet) ui_notify(UI_ERROR, "amivcs: '%s' goes back further than the history", vcs_shown(rev));
            return 0;
        }
    }
    return 1;
}
