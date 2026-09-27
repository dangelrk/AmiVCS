/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * packfmt.c -- see packfmt.h. Read-only: this file only ever looks
 * objects up in existing packs. pack.c owns writing them.
 */
#include "packfmt.h"
#include "varint.h"
#include "localfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { VcsOid oid; unsigned long offset; unsigned long len; } PackIdxEntry;

static int read_whole_file(const char *path, unsigned char **out, long *out_len)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    unsigned char *buf;

    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return 0; }

    buf = (unsigned char *)malloc(sz ? (size_t)sz : 1);
    if (!buf) { fclose(fp); return 0; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); free(buf); return 0; }
    fclose(fp);

    *out = buf;
    *out_len = sz;
    return 1;
}

static int parse_idx(const unsigned char *buf, long len, PackIdxEntry **out_entries, int *out_n)
{
    size_t pos, remaining;
    unsigned long count, i;
    size_t used;
    PackIdxEntry *entries;

    if (len < 1 || buf[0] != VCS_FORMAT_VERSION) return 0;
    pos = 1;
    remaining = (size_t)len - 1;

    used = vcs_varint_read(buf + pos, remaining, &count);
    if (!used) return 0;
    pos += used; remaining -= used;

    entries = (PackIdxEntry *)malloc(sizeof(PackIdxEntry) * (count ? count : 1));
    if (!entries) return 0;

    for (i = 0; i < count; i++) {
        unsigned long offset, elen;

        if (remaining < VCS_OID_SIZE) { free(entries); return 0; }
        memcpy(entries[i].oid.b, buf + pos, VCS_OID_SIZE);
        pos += VCS_OID_SIZE; remaining -= VCS_OID_SIZE;

        used = vcs_varint_read(buf + pos, remaining, &offset);
        if (!used) { free(entries); return 0; }
        pos += used; remaining -= used;

        used = vcs_varint_read(buf + pos, remaining, &elen);
        if (!used) { free(entries); return 0; }
        pos += used; remaining -= used;

        entries[i].offset = offset;
        entries[i].len = elen;
    }
    *out_entries = entries;
    *out_n = (int)count;
    return 1;
}

static int idx_find(const PackIdxEntry *entries, int n, const VcsOid *oid, unsigned long *offset_out)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int cmp = memcmp(entries[mid].oid.b, oid->b, VCS_OID_SIZE);
        if (cmp == 0) { *offset_out = entries[mid].offset; return 1; }
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static int read_record_at(const char *pack_path, unsigned long offset, VcsObjType *type_out, void **data_out, long *len_out)
{
    FILE *pf;
    unsigned char hdr[1 + 10];
    size_t got, used;
    unsigned long payload_len;
    void *payload;
    int ok = 0;

    pf = fopen(pack_path, "rb");
    if (!pf) return 0;
    if (fseek(pf, (long)offset, SEEK_SET) != 0) { fclose(pf); return 0; }

    got = fread(hdr, 1, sizeof(hdr), pf);
    if (got < 1) { fclose(pf); return 0; }

    used = vcs_varint_read(hdr + 1, got - 1, &payload_len);
    if (!used) { fclose(pf); return 0; }

    if (fseek(pf, (long)(offset + 1 + used), SEEK_SET) != 0) { fclose(pf); return 0; }

    payload = malloc(payload_len ? payload_len : 1);
    if (payload && (payload_len == 0 || fread(payload, 1, payload_len, pf) == payload_len)) {
        *type_out = (VcsObjType)hdr[0];
        *data_out = payload;
        *len_out = (long)payload_len;
        ok = 1;
    } else {
        free(payload);
    }
    fclose(pf);
    return ok;
}

int vcs_pack_lookup(const VcsObjStore *store, const VcsOid *oid, VcsObjType *type_out, void **data_out, long *len_out)
{
    char objects_dir[1024], pack_dir[1024];
    LocalDir *dir;
    LocalDirEntry de;
    int found = 0;

    localfs_join(objects_dir, sizeof(objects_dir), store->root, "objects");
    localfs_join(pack_dir, sizeof(pack_dir), objects_dir, VCS_PACK_DIR);

    dir = localfs_opendir(pack_dir);
    if (!dir) return 0;

    while (!found && localfs_readdir(dir, &de)) {
        size_t nlen = strlen(de.name);
        if (de.is_dir || nlen < 5 || strcmp(de.name + nlen - 4, ".idx") != 0) continue;

        {
            char idx_path[1024];
            unsigned char *idx_buf;
            long idx_len;
            PackIdxEntry *entries;
            int n;

            localfs_join(idx_path, sizeof(idx_path), pack_dir, de.name);
            if (!read_whole_file(idx_path, &idx_buf, &idx_len)) continue;
            if (!parse_idx(idx_buf, idx_len, &entries, &n)) { free(idx_buf); continue; }
            free(idx_buf);

            {
                unsigned long offset;
                if (idx_find(entries, n, oid, &offset)) {
                    char base[256], packname[300], pack_path[1024];
                    size_t blen = nlen - 4;
                    if (blen >= sizeof(base)) blen = sizeof(base) - 1;
                    memcpy(base, de.name, blen);
                    base[blen] = '\0';
                    snprintf(packname, sizeof(packname), "%s.pack", base);
                    localfs_join(pack_path, sizeof(pack_path), pack_dir, packname);
                    found = read_record_at(pack_path, offset, type_out, data_out, len_out);
                }
                free(entries);
            }
        }
    }
    localfs_closedir(dir);
    return found;
}
