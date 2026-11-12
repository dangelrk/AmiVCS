/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * pack.c -- see pack.h and packfmt.h (the shared on-disk format both this
 * writer and objstore.c's read fallback agree on).
 */
#include "pack.h"
#include "packfmt.h"
#include "varint.h"
#include "localfs.h"
#include "md5.h"
#include "ui_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COPY_CHUNK 8192

/* hex characters of the pack's MD5 in its file name: "<24>.pack" is 29
 * characters and fits a classic FFS name (30) */
#define VCS_PACK_HEX_CHARS 24

/* size of a file in bytes, -1 if it cannot be opened */
static long file_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    sz = ftell(fp);
    fclose(fp);
    return sz;
}

static void loose_path_for(const char *objects_dir, const VcsOid *oid, char *out, size_t sz)
{
    char hex[VCS_OID_HEX], fandir[1024];
    char fan[3];
    vcs_oid_to_hex(oid, hex);
    fan[0] = hex[0]; fan[1] = hex[1]; fan[2] = '\0';
    localfs_join(fandir, sizeof(fandir), objects_dir, fan);
    localfs_join(out, sz, fandir, hex + 2);
}

/* Enumerates every loose object under <objects_dir>/<2-hex>/<30-hex>,
 * skipping the "pack" subdirectory itself. *out_list is malloc'd (an
 * array of VcsOid), caller frees. Returns 1 on success (even if empty). */
static int collect_loose(const char *objects_dir, VcsOid **out_list, int *out_n)
{
    LocalDir *topdir;
    LocalDirEntry tde;
    VcsOid *list = NULL;
    int n = 0, cap = 0;

    topdir = localfs_opendir(objects_dir);
    if (!topdir) { *out_list = NULL; *out_n = 0; return 1; }

    while (localfs_readdir(topdir, &tde)) {
        if (!tde.is_dir || strlen(tde.name) != 2) continue; /* skips "pack" */

        {
            char subdir[1024];
            LocalDir *sd;
            LocalDirEntry sde;

            localfs_join(subdir, sizeof(subdir), objects_dir, tde.name);
            sd = localfs_opendir(subdir);
            if (!sd) continue;

            while (localfs_readdir(sd, &sde)) {
                char hex[VCS_OID_HEX];
                VcsOid oid;

                if (sde.is_dir || strlen(sde.name) != VCS_OID_HEX - 3) continue;
                snprintf(hex, sizeof(hex), "%s%s", tde.name, sde.name);
                if (!vcs_oid_from_hex(hex, &oid)) continue;

                if (n == cap) {
                    int newcap = cap ? cap * 2 : 64;
                    VcsOid *tmp = (VcsOid *)realloc(list, sizeof(VcsOid) * (size_t)newcap);
                    if (!tmp) { localfs_closedir(sd); localfs_closedir(topdir); free(list); return 0; }
                    list = tmp;
                    cap = newcap;
                }
                list[n++] = oid;
            }
            localfs_closedir(sd);
        }
    }
    localfs_closedir(topdir);

    *out_list = list;
    *out_n = n;
    return 1;
}

static int loose_oid_cmp(const void *a, const void *b)
{
    return memcmp(((const VcsOid *)a)->b, ((const VcsOid *)b)->b, VCS_OID_SIZE);
}

int vcs_pack_gc(VcsObjStore *store, int *out_count)
{
    char objects_dir[1024], pack_dir[1024];
    VcsOid *loose;
    int n, i, packed_n = 0;
    VcsOid *packed_oids = NULL;
    unsigned long *packed_offsets = NULL, *packed_lens = NULL;

    if (out_count) *out_count = 0;

    localfs_join(objects_dir, sizeof(objects_dir), store->root, "objects");
    localfs_join(pack_dir, sizeof(pack_dir), objects_dir, VCS_PACK_DIR);
    localfs_mkdir_p(pack_dir);

    if (!collect_loose(objects_dir, &loose, &n)) return 0;
    if (n == 0) { free(loose); return 1; }

    qsort(loose, (size_t)n, sizeof(VcsOid), loose_oid_cmp);

    packed_oids = (VcsOid *)malloc(sizeof(VcsOid) * (size_t)n);
    packed_offsets = (unsigned long *)malloc(sizeof(unsigned long) * (size_t)n);
    packed_lens = (unsigned long *)malloc(sizeof(unsigned long) * (size_t)n);
    if (!packed_oids || !packed_offsets || !packed_lens) {
        free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
        return 0;
    }

    {
        char tmp_pack[1024];
        long pack_size = -1;
        FILE *out;
        unsigned char hdr[1 + 10];
        size_t hdr_len;

        localfs_join(tmp_pack, sizeof(tmp_pack), pack_dir, ".tmp-pack");
        out = fopen(tmp_pack, "wb");
        if (!out) { free(loose); free(packed_oids); free(packed_offsets); free(packed_lens); return 0; }

        hdr[0] = VCS_FORMAT_VERSION;
        hdr_len = 1 + vcs_varint_write(hdr + 1, (unsigned long)n);
        if (fwrite(hdr, 1, hdr_len, out) != hdr_len) {
            fclose(out); remove(tmp_pack);
            free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
            return 0;
        }

        for (i = 0; i < n; i++) {
            char loose_file[1024];
            FILE *in;
            unsigned char lhdr[1 + 1 + 10];
            size_t got, used;
            unsigned char ltype;
            unsigned long lpaylen;
            long offset;
            unsigned char rhdr[1 + 10];
            size_t rhdr_len;
            int copy_ok;

            loose_path_for(objects_dir, &loose[i], loose_file, sizeof(loose_file));
            in = fopen(loose_file, "rb");
            if (!in) continue; /* vanished under us -- skip, not fatal */

            got = fread(lhdr, 1, sizeof(lhdr), in);
            if (got < 2) { fclose(in); continue; }
            ltype = lhdr[1];
            used = vcs_varint_read(lhdr + 2, got - 2, &lpaylen);
            if (!used || fseek(in, (long)(2 + used), SEEK_SET) != 0) { fclose(in); continue; }

            offset = ftell(out);
            rhdr[0] = ltype;
            rhdr_len = 1 + vcs_varint_write(rhdr + 1, lpaylen);
            if (fwrite(rhdr, 1, rhdr_len, out) != rhdr_len) { fclose(in); continue; }

            copy_ok = 1;
            {
                unsigned char buf[COPY_CHUNK];
                unsigned long remaining = lpaylen;
                while (remaining > 0) {
                    size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
                    size_t r = fread(buf, 1, chunk, in);
                    if (r == 0) { copy_ok = 0; break; }
                    if (fwrite(buf, 1, r, out) != r) { copy_ok = 0; break; }
                    remaining -= (unsigned long)r;
                }
            }
            fclose(in);
            if (!copy_ok) continue;

            packed_oids[packed_n] = loose[i];
            packed_offsets[packed_n] = (unsigned long)offset;
            packed_lens[packed_n] = lpaylen;
            packed_n++;
        }
        pack_size = ftell(out);
        if (fclose(out) != 0) pack_size = -1;

        if (packed_n == 0) {
            remove(tmp_pack);
            free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
            return 1;
        }

        {
            char pack_hex[VCS_OID_HEX], packname[300], final_pack[1024];

            if (!md5_file_hex(tmp_pack, pack_hex)) {
                remove(tmp_pack);
                free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
                return 0;
            }
            /* "<24 hex>.pack"/".idx" (29/28 chars): classic FFS keeps only
             * 30 and truncates silently -- the old "pack-<32 hex>.pack" and
             * ".idx" both became the same 30-char name there, so the idx
             * replaced the pack and the loose objects were pruned after it.
             * Readers find packs through any "*.idx" (packfmt.c), so old
             * "pack-..." packs stay readable and old versions read these. */
            snprintf(packname, sizeof(packname), "%.*s.pack", VCS_PACK_HEX_CHARS, pack_hex);
            localfs_join(final_pack, sizeof(final_pack), pack_dir, packname);
            /* AmigaDOS Rename() fails outright if the destination
             * already exists (see dos.doc's Rename() autodoc) -- unlike
             * POSIX rename(), it never replaces an existing target.
             * final_pack's name is content-hash-derived, so this is
             * rare (only re-running gc on an unchanged loose-object set
             * reproduces the same hash), but not impossible -- delete
             * any stale target first; harmless if there isn't one. */
            remove(final_pack);
            if (rename(tmp_pack, final_pack) != 0) {
                remove(tmp_pack);
                free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
                return 0;
            }

            {
                char tmp_idx[1024], final_idx[1024];
                FILE *ifp;
                unsigned char ihdr[1 + 10];
                size_t ihdr_len;
                int ok = 1;

                localfs_join(tmp_idx, sizeof(tmp_idx), pack_dir, ".tmp-idx");
                ifp = fopen(tmp_idx, "wb");
                if (!ifp) { free(loose); free(packed_oids); free(packed_offsets); free(packed_lens); return 0; }

                ihdr[0] = VCS_FORMAT_VERSION;
                ihdr_len = 1 + vcs_varint_write(ihdr + 1, (unsigned long)packed_n);
                if (fwrite(ihdr, 1, ihdr_len, ifp) != ihdr_len) ok = 0;

                for (i = 0; ok && i < packed_n; i++) {
                    unsigned char ebuf[VCS_OID_SIZE + 20];
                    size_t epos = 0;
                    memcpy(ebuf, packed_oids[i].b, VCS_OID_SIZE);
                    epos += VCS_OID_SIZE;
                    epos += vcs_varint_write(ebuf + epos, packed_offsets[i]);
                    epos += vcs_varint_write(ebuf + epos, packed_lens[i]);
                    if (fwrite(ebuf, 1, epos, ifp) != epos) ok = 0;
                }
                fclose(ifp);

                if (!ok) {
                    remove(tmp_idx);
                    free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
                    return 0;
                }

                snprintf(packname, sizeof(packname), "%.*s.idx", VCS_PACK_HEX_CHARS, pack_hex);
                localfs_join(final_idx, sizeof(final_idx), pack_dir, packname);
                /* Same AmigaDOS Rename()-onto-existing-target caveat as
                 * final_pack above. */
                remove(final_idx);
                if (rename(tmp_idx, final_idx) != 0) {
                    remove(tmp_idx);
                    free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
                    return 0;
                }
                /* Both must be on disk as two files of the sizes just
                 * written before a single loose object goes: a file system
                 * that mangles names (truncation, a reserved name) shows
                 * here, and then everything stays loose. */
                if (pack_size < 0 || file_size(final_pack) != pack_size
                    || file_size(final_idx) <= 0 || strcmp(final_pack, final_idx) == 0) {
                    ui_notify(UI_ERROR, "amivcs: gc: pack %s is not on disk as written -- "
                              "loose objects kept", final_pack);
                    free(loose); free(packed_oids); free(packed_offsets); free(packed_lens);
                    return 0;
                }
            }
        }
    }

    /* Prune: only the loose copies that are now safely inside the pack. */
    for (i = 0; i < packed_n; i++) {
        char loose_file[1024], hex[VCS_OID_HEX], fan[3], fandir[1024];
        loose_path_for(objects_dir, &packed_oids[i], loose_file, sizeof(loose_file));
        localfs_remove(loose_file);
        vcs_oid_to_hex(&packed_oids[i], hex);
        fan[0] = hex[0]; fan[1] = hex[1]; fan[2] = '\0';
        localfs_join(fandir, sizeof(fandir), objects_dir, fan);
        localfs_remove(fandir); /* best-effort: only succeeds once empty */
    }

    if (out_count) *out_count = packed_n;
    free(loose);
    free(packed_oids);
    free(packed_offsets);
    free(packed_lens);
    return 1;
}
