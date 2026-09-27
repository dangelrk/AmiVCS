/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * refs.c -- see refs.h. Plain-text packed-refs (one "<oid> <name>" line
 * each) and a small HEAD file, both read/written with fopen/fgets --
 * small enough that a hand-rolled line parser is simpler than pulling in
 * parson for two columns of data.
 */
#include "refs.h"
#include "localfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vcs_refs_free(VcsRefList *refs)
{
    free(refs->items);
    refs->items = NULL;
    refs->n = refs->cap = 0;
}

static void refs_init(VcsRefList *refs)
{
    refs->items = NULL;
    refs->n = 0;
    refs->cap = 0;
}

int vcs_refs_load(const char *amivcs_dir, VcsRefList *out)
{
    char path[1024];
    FILE *fp;
    char line[512];

    refs_init(out);
    localfs_join(path, sizeof(path), amivcs_dir, VCS_PACKED_REFS_FILE);

    fp = fopen(path, "r");
    if (!fp) return 1; /* no refs yet -- not an error */

    while (fgets(line, sizeof(line), fp)) {
        char hex[VCS_OID_HEX];
        char name[128];
        VcsOid oid;

        /* "<32-hex> <name>\n" */
        if (sscanf(line, "%32s %127s", hex, name) != 2) continue;
        if (!vcs_oid_from_hex(hex, &oid)) continue;
        vcs_refs_upsert(out, name, &oid);
    }
    fclose(fp);
    return 1;
}

int vcs_refs_save(const char *amivcs_dir, const VcsRefList *refs)
{
    char path[1024], tmp_path[1040];
    FILE *fp;
    int i;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_PACKED_REFS_FILE);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    fp = fopen(tmp_path, "w");
    if (!fp) return 0;

    for (i = 0; i < refs->n; i++) {
        char hex[VCS_OID_HEX];
        vcs_oid_to_hex(&refs->items[i].oid, hex);
        if (fprintf(fp, "%s %s\n", hex, refs->items[i].name) < 0) {
            fclose(fp); remove(tmp_path); return 0;
        }
    }
    fclose(fp);

    /* AmigaDOS Rename() fails outright if the destination already
     * exists (see dos.doc's Rename() autodoc) -- unlike POSIX rename(),
     * it never replaces an existing target. Without this, only the
     * very first ref save (packed-refs doesn't exist yet) would ever
     * succeed; every commit after that would silently fail to move the
     * branch pointer (see libcloudcore's config_save(), which had the
     * identical bug -- same fix, same reasoning). Delete the old target
     * first; if there isn't one yet (first commit), this simply fails
     * harmlessly. */
    remove(path);
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return 0; }
    return 1;
}

VcsRef *vcs_refs_find(VcsRefList *refs, const char *name)
{
    int i;
    for (i = 0; i < refs->n; i++) {
        if (strcmp(refs->items[i].name, name) == 0) return &refs->items[i];
    }
    return NULL;
}

void vcs_refs_upsert(VcsRefList *refs, const char *name, const VcsOid *oid)
{
    VcsRef *r = vcs_refs_find(refs, name);
    if (r) { r->oid = *oid; return; }

    if (refs->n == refs->cap) {
        int newcap = refs->cap ? refs->cap * 2 : 8;
        VcsRef *tmp = (VcsRef *)realloc(refs->items, sizeof(VcsRef) * (size_t)newcap);
        if (!tmp) return;
        refs->items = tmp;
        refs->cap = newcap;
    }
    r = &refs->items[refs->n++];
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->oid = *oid;
}

void vcs_refs_remove(VcsRefList *refs, const char *name)
{
    int i;
    for (i = 0; i < refs->n; i++) {
        if (strcmp(refs->items[i].name, name) == 0) {
            refs->items[i] = refs->items[refs->n - 1];
            refs->n--;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */

int vcs_head_load(const char *amivcs_dir, VcsHead *out)
{
    char path[1024];
    FILE *fp;
    char line[256];

    localfs_join(path, sizeof(path), amivcs_dir, VCS_HEAD_FILE);
    fp = fopen(path, "r");
    if (!fp) return 0;

    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    fclose(fp);

    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    }

    if (strncmp(line, "ref: " VCS_REF_PREFIX, strlen("ref: " VCS_REF_PREFIX)) == 0) {
        out->detached = 0;
        strncpy(out->branch, line + strlen("ref: " VCS_REF_PREFIX), sizeof(out->branch) - 1);
        out->branch[sizeof(out->branch) - 1] = '\0';
        return 1;
    }

    if (vcs_oid_from_hex(line, &out->oid)) {
        out->detached = 1;
        out->branch[0] = '\0';
        return 1;
    }

    return 0; /* malformed HEAD */
}

int vcs_head_set_branch(const char *amivcs_dir, const char *branch)
{
    char path[1024];
    FILE *fp;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_HEAD_FILE);
    fp = fopen(path, "w");
    if (!fp) return 0;
    fprintf(fp, "ref: %s%s\n", VCS_REF_PREFIX, branch);
    fclose(fp);
    return 1;
}

int vcs_head_set_detached(const char *amivcs_dir, const VcsOid *oid)
{
    char path[1024], hex[VCS_OID_HEX];
    FILE *fp;

    localfs_join(path, sizeof(path), amivcs_dir, VCS_HEAD_FILE);
    fp = fopen(path, "w");
    if (!fp) return 0;
    vcs_oid_to_hex(oid, hex);
    fprintf(fp, "%s\n", hex);
    fclose(fp);
    return 1;
}

int vcs_head_resolve(const char *amivcs_dir, VcsOid *out_oid)
{
    VcsHead head;
    if (!vcs_head_load(amivcs_dir, &head)) return 0;

    if (head.detached) {
        *out_oid = head.oid;
        return 1;
    }

    {
        VcsRefList refs;
        char fullname[128];
        VcsRef *r;

        if (!vcs_refs_load(amivcs_dir, &refs)) return 0;
        snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, head.branch);
        r = vcs_refs_find(&refs, fullname);
        *out_oid = r ? r->oid : VCS_OID_ZERO;
        vcs_refs_free(&refs);
    }
    return 1;
}
