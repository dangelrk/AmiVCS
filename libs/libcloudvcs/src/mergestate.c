/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * mergestate.c -- see mergestate.h.
 */
#include "mergestate.h"
#include "localfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vcs_mergestate_init(VcsMergeState *st)
{
    memset(st, 0, sizeof(*st));
}

void vcs_mergestate_free(VcsMergeState *st)
{
    int i;
    for (i = 0; i < st->n; i++) free(st->items[i].path);
    free(st->items);
    vcs_mergestate_init(st);
}

VcsMergeFile *vcs_mergestate_find(VcsMergeState *st, const char *path)
{
    int i;
    for (i = 0; i < st->n; i++)
        if (strcmp(st->items[i].path, path) == 0) return &st->items[i];
    return NULL;
}

int vcs_mergestate_is_conflict(const VcsMergeFile *f)
{
    return strcmp(f->kind, "touched") != 0;
}

VcsMergeFile *vcs_mergestate_put(VcsMergeState *st, char status, const char *kind,
                                 int theirs_file, const char *path)
{
    VcsMergeFile *f = vcs_mergestate_find(st, path);
    if (!f) {
        if (st->n == st->cap) {
            int newcap = st->cap ? st->cap * 2 : 16;
            VcsMergeFile *tmp = (VcsMergeFile *)realloc(st->items, sizeof(VcsMergeFile) * (size_t)newcap);
            if (!tmp) return NULL;
            st->items = tmp;
            st->cap = newcap;
        }
        f = &st->items[st->n];
        f->path = (char *)malloc(strlen(path) + 1);
        if (!f->path) return NULL;
        strcpy(f->path, path);
        st->n++;
    } else if (strcmp(kind, "touched") == 0 && vcs_mergestate_is_conflict(f)) {
        return f;   /* a conflict stays a conflict */
    }
    f->status = status;
    strncpy(f->kind, kind, sizeof(f->kind) - 1);
    f->kind[sizeof(f->kind) - 1] = '\0';
    f->theirs_file = theirs_file;
    return f;
}

int vcs_mergestate_unresolved(const VcsMergeState *st)
{
    int i, n = 0;
    for (i = 0; i < st->n; i++)
        if (st->items[i].status == 'U') n++;
    return n;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

int vcs_mergestate_load(const char *amivcs_dir, VcsMergeState *st)
{
    char path[1024], line[1400];
    FILE *fp;

    vcs_mergestate_init(st);
    localfs_join(path, sizeof(path), amivcs_dir, VCS_MERGE_HEAD_FILE);
    fp = fopen(path, "r");
    if (!fp) return 1;   /* no merge in progress */

    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    chomp(line);
    if (!vcs_oid_from_hex(line, &st->theirs)) { fclose(fp); return 0; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    chomp(line);
    if (!vcs_oid_from_hex(line, &st->ours)) { fclose(fp); return 0; }
    if (fgets(line, sizeof(line), fp)) {
        chomp(line);
        strncpy(st->label, line, sizeof(st->label) - 1);
    }
    fclose(fp);
    st->active = 1;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_MERGE_FILES_FILE);
    fp = fopen(path, "r");
    if (!fp) {
        /* a save interrupted between removing the old file and renaming
         * the new one: the new one is complete in the .tmp */
        char tmp[1040];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        fp = fopen(tmp, "r");
    }
    if (!fp) return 1;
    while (fgets(line, sizeof(line), fp)) {
        char *kind, *flags, *p;
        chomp(line);
        if (!line[0] || line[1] != '\t') continue;
        kind = line + 2;
        flags = strchr(kind, '\t');
        if (!flags) continue;
        *flags++ = '\0';
        p = strchr(flags, '\t');
        if (!p) continue;
        *p++ = '\0';
        if (!*p) continue;
        vcs_mergestate_put(st, line[0], kind, strchr(flags, 't') != NULL, p);
    }
    fclose(fp);
    return 1;
}

/* tmp + rename, the old target removed first (AmigaDOS Rename() never
 * replaces, see vcs_refs_save()) */
static int replace_file(const char *tmp, const char *path)
{
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return 0; }
    return 1;
}

int vcs_mergestate_save(const char *amivcs_dir, const VcsMergeState *st)
{
    char path[1024], tmp[1040], hex[VCS_OID_HEX];
    FILE *fp;
    int i, ok = 1;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_MERGE_FILES_FILE);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (!fp) return 0;
    for (i = 0; i < st->n && ok; i++) {
        const VcsMergeFile *f = &st->items[i];
        if (fprintf(fp, "%c\t%s\t%s\t%s\n", f->status, f->kind, f->theirs_file ? "t" : "-", f->path) < 0)
            ok = 0;
    }
    if (fclose(fp) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
    if (!replace_file(tmp, path)) return 0;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_MERGE_HEAD_FILE);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (!fp) return 0;
    vcs_oid_to_hex(&st->theirs, hex);
    if (fprintf(fp, "%s\n", hex) < 0) ok = 0;
    vcs_oid_to_hex(&st->ours, hex);
    if (fprintf(fp, "%s\n%s\n", hex, st->label) < 0) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
    return replace_file(tmp, path);
}

void vcs_mergestate_clear(const char *amivcs_dir)
{
    char path[1024];
    localfs_join(path, sizeof(path), amivcs_dir, VCS_MERGE_HEAD_FILE);
    remove(path);
    localfs_join(path, sizeof(path), amivcs_dir, VCS_MERGE_FILES_FILE);
    remove(path);
}

int vcs_mergestate_active(const char *amivcs_dir)
{
    char path[1024];
    localfs_join(path, sizeof(path), amivcs_dir, VCS_MERGE_HEAD_FILE);
    return localfs_exists(path);
}

static int under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 0) return 1;
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

int vcs_mergestate_mark_resolved(const char *amivcs_dir, const char *relpath)
{
    VcsMergeState st;
    int i, changed = 0, ok = 1;

    if (!vcs_mergestate_active(amivcs_dir)) return 1;
    if (!vcs_mergestate_load(amivcs_dir, &st)) return 0;
    for (i = 0; i < st.n; i++) {
        if (st.items[i].status == 'U' && under(st.items[i].path, relpath)) {
            st.items[i].status = 'R';
            changed = 1;
        }
    }
    if (changed) ok = vcs_mergestate_save(amivcs_dir, &st);
    vcs_mergestate_free(&st);
    return ok;
}
