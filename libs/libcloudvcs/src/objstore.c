/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * objstore.c -- see objstore.h. Loose-object storage: fan-out directories,
 * atomic write-then-rename (never leaves a half-written object at its
 * final, content-addressed path), streamed blob I/O so a file's whole
 * content never has to fit in RAM at once.
 */
#include "objstore.h"
#include "packfmt.h"
#include "varint.h"
#include "md5.h"
#include "localfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRITE_CHUNK 8192

void vcsobjstore_init(VcsObjStore *st, const char *amivcs_dir)
{
    strncpy(st->root, amivcs_dir, sizeof(st->root) - 1);
    st->root[sizeof(st->root) - 1] = '\0';
}

static void objects_dir(const VcsObjStore *st, char *out, size_t sz)
{
    localfs_join(out, sz, st->root, "objects");
}

int vcsobjstore_create_dirs(const VcsObjStore *st)
{
    char objdir[1024], packdir[1040];
    objects_dir(st, objdir, sizeof(objdir));
    if (!localfs_mkdir_p(objdir)) return 0;
    localfs_join(packdir, sizeof(packdir), objdir, "pack");
    return localfs_mkdir_p(packdir);
}

/* <root>/objects/<xx>/<30 remaining hex chars> */
static void oid_path(const VcsObjStore *st, const VcsOid *oid, char *out, size_t sz)
{
    char hex[VCS_OID_HEX];
    char objdir[1024], fandir[1040];
    vcs_oid_to_hex(oid, hex);
    objects_dir(st, objdir, sizeof(objdir));
    /* 2-char fan-out prefix as its own path segment */
    {
        char prefix[3];
        prefix[0] = hex[0]; prefix[1] = hex[1]; prefix[2] = '\0';
        localfs_join(fandir, sizeof(fandir), objdir, prefix);
    }
    localfs_join(out, sz, fandir, hex + 2);
}

static void tmp_path(const VcsObjStore *st, char *out, size_t sz)
{
    char objdir[1024];
    objects_dir(st, objdir, sizeof(objdir));
    /* Fixed name: objstore writes are not designed for concurrent AMIVCS
     * processes on the same repo (plan section 12, "Repo-Lock" -- a
     * future single lock file, not yet implemented). */
    localfs_join(out, sz, objdir, ".tmp-write");
}

int vcsobjstore_exists(const VcsObjStore *st, const VcsOid *oid)
{
    char path[1024];
    VcsObjType type;
    void *data;
    long len;

    oid_path(st, oid, path, sizeof(path));
    if (localfs_exists(path)) return 1;

    /* Not loose -- check packs too, but without paying for a full read:
     * vcs_pack_lookup() has no "exists only" mode, so this still does the
     * work of pulling the payload into RAM just to free it again. Rare
     * call path (write-time dedup checks are the only caller, and gc
     * itself never touches already-packed objects), so not worth a
     * second pack-scanning routine just to shave that cost. */
    if (vcs_pack_lookup(st, oid, &type, &data, &len)) { free(data); return 1; }
    return 0;
}

static int finalize_tmp(const VcsObjStore *st, const char *tmp, const VcsOid *oid)
{
    char final[1024], fandir[1040], hex[VCS_OID_HEX], objdir[1024];

    if (vcsobjstore_exists(st, oid)) {
        /* Dedup: identical content already stored -- discard the temp
         * copy instead of overwriting (loose files are content-addressed,
         * so the existing one is byte-identical already). */
        remove(tmp);
        return 1;
    }

    vcs_oid_to_hex(oid, hex);
    objects_dir(st, objdir, sizeof(objdir));
    {
        char prefix[3];
        prefix[0] = hex[0]; prefix[1] = hex[1]; prefix[2] = '\0';
        localfs_join(fandir, sizeof(fandir), objdir, prefix);
    }
    if (!localfs_mkdir(fandir)) return 0;

    oid_path(st, oid, final, sizeof(final));
    if (rename(tmp, final) != 0) { remove(tmp); return 0; }
    return 1;
}

int vcsobjstore_write(VcsObjStore *st, VcsObjType type, const void *data, long len, VcsOid *out_oid)
{
    char hex_hash[VCS_OID_HEX];
    VcsOid oid;
    char tmp[1024];
    FILE *fp;
    unsigned char header[1 + 1 + 10];
    size_t hdr_len;

    md5_buffer_hex(data, (size_t)len, hex_hash);
    if (!vcs_oid_from_hex(hex_hash, &oid)) return 0;

    tmp_path(st, tmp, sizeof(tmp));
    fp = fopen(tmp, "wb");
    if (!fp) return 0;

    header[0] = VCS_FORMAT_VERSION;
    header[1] = (unsigned char)type;
    hdr_len = 2 + vcs_varint_write(header + 2, (unsigned long)len);
    if (fwrite(header, 1, hdr_len, fp) != hdr_len) { fclose(fp); remove(tmp); return 0; }
    if (len > 0 && fwrite(data, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); remove(tmp); return 0; }
    fclose(fp);

    if (!finalize_tmp(st, tmp, &oid)) return 0;
    *out_oid = oid;
    return 1;
}

int vcsobjstore_write_blob_file(VcsObjStore *st, const char *local_path, VcsOid *out_oid)
{
    long size, mtime;
    FILE *in, *out;
    Md5Ctx ctx;
    unsigned char buf[WRITE_CHUNK];
    unsigned char header[1 + 1 + 10];
    size_t hdr_len;
    char tmp[1024];
    VcsOid oid;
    long remaining;

    if (!localfs_stat(local_path, &size, &mtime)) return 0;

    in = fopen(local_path, "rb");
    if (!in) return 0;

    tmp_path(st, tmp, sizeof(tmp));
    out = fopen(tmp, "wb");
    if (!out) { fclose(in); return 0; }

    header[0] = VCS_FORMAT_VERSION;
    header[1] = (unsigned char)VCS_OBJ_BLOB;
    hdr_len = 2 + vcs_varint_write(header + 2, (unsigned long)size);
    if (fwrite(header, 1, hdr_len, out) != hdr_len) {
        fclose(in); fclose(out); remove(tmp); return 0;
    }

    md5_init(&ctx);
    remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining > (long)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        size_t got = fread(buf, 1, chunk, in);
        if (got == 0) break;
        md5_update(&ctx, buf, got);
        if (fwrite(buf, 1, got, out) != got) {
            fclose(in); fclose(out); remove(tmp); return 0;
        }
        remaining -= (long)got;
    }
    fclose(in);
    fclose(out);
    if (remaining != 0) { remove(tmp); return 0; } /* file changed size mid-read */

    {
        unsigned char digest[16];
        md5_final(&ctx, digest);
        memcpy(oid.b, digest, VCS_OID_SIZE);
    }

    if (!finalize_tmp(st, tmp, &oid)) return 0;
    *out_oid = oid;
    return 1;
}

int vcsobjstore_read(const VcsObjStore *st, const VcsOid *oid, VcsObjType *type_out, void **data_out, long *len_out)
{
    char path[1024];
    FILE *fp;
    unsigned char header[1 + 1 + 10];
    size_t got;
    unsigned char version, type;
    unsigned long payload_len;
    size_t used;
    void *payload;

    oid_path(st, oid, path, sizeof(path));
    fp = fopen(path, "rb");
    if (!fp) return vcs_pack_lookup(st, oid, type_out, data_out, len_out);

    got = fread(header, 1, sizeof(header), fp);
    if (got < 2) { fclose(fp); return 0; }
    version = header[0];
    type = header[1];
    if (version != VCS_FORMAT_VERSION) { fclose(fp); return 0; }

    used = vcs_varint_read(header + 2, got - 2, &payload_len);
    if (!used) { fclose(fp); return 0; }

    /* Rewind to right after the header we actually consumed, since the
     * fixed-size read above may have slurped a few payload bytes too. */
    if (fseek(fp, (long)(2 + used), SEEK_SET) != 0) { fclose(fp); return 0; }

    payload = malloc(payload_len ? payload_len : 1);
    if (!payload) { fclose(fp); return 0; }
    if (payload_len > 0 && fread(payload, 1, payload_len, fp) != payload_len) {
        fclose(fp); free(payload); return 0;
    }
    fclose(fp);

    *type_out = (VcsObjType)type;
    *data_out = payload;
    *len_out = (long)payload_len;
    return 1;
}

int vcsobjstore_extract_blob_to_file(const VcsObjStore *st, const VcsOid *oid, const char *local_path)
{
    char path[1024];
    FILE *in, *out;
    unsigned char header[1 + 1 + 10];
    size_t got, used;
    unsigned char version, type;
    unsigned long payload_len, remaining;
    unsigned char buf[WRITE_CHUNK];

    oid_path(st, oid, path, sizeof(path));
    in = fopen(path, "rb");
    if (!in) {
        /* Packed, not loose: no streaming path into a pack record (v1
         * simplification -- gc'd blobs are read fully into RAM here,
         * unlike the streamed loose-object path above; acceptable, this
         * only affects checking out an already-packed blob). */
        VcsObjType type;
        void *data;
        long len;
        FILE *out;
        int ok;

        if (!vcs_pack_lookup(st, oid, &type, &data, &len) || type != VCS_OBJ_BLOB) return 0;
        out = fopen(local_path, "wb");
        if (!out) { free(data); return 0; }
        ok = (len == 0 || fwrite(data, 1, (size_t)len, out) == (size_t)len);
        fclose(out);
        free(data);
        return ok;
    }

    got = fread(header, 1, sizeof(header), in);
    if (got < 2) { fclose(in); return 0; }
    version = header[0];
    type = header[1];
    if (version != VCS_FORMAT_VERSION || type != VCS_OBJ_BLOB) { fclose(in); return 0; }

    used = vcs_varint_read(header + 2, got - 2, &payload_len);
    if (!used) { fclose(in); return 0; }
    if (fseek(in, (long)(2 + used), SEEK_SET) != 0) { fclose(in); return 0; }

    out = fopen(local_path, "wb");
    if (!out) { fclose(in); return 0; }

    remaining = payload_len;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        size_t r = fread(buf, 1, chunk, in);
        if (r == 0) break;
        if (fwrite(buf, 1, r, out) != r) { fclose(in); fclose(out); return 0; }
        remaining -= (unsigned long)r;
    }
    fclose(in);
    fclose(out);
    return remaining == 0;
}

int vcsobjstore_read_blob_max(const VcsObjStore *st, const VcsOid *oid, long max_len,
                              void **data_out, long *len_out)
{
    char path[1024];
    FILE *fp;
    VcsObjType type;

    *data_out = NULL;
    *len_out = 0;
    oid_path(st, oid, path, sizeof(path));
    fp = fopen(path, "rb");
    if (fp) {
        unsigned char header[1 + 1 + 10];
        unsigned long payload_len;
        size_t got = fread(header, 1, sizeof(header), fp), used;
        fclose(fp);
        if (got < 2 || header[0] != VCS_FORMAT_VERSION || header[1] != (unsigned char)VCS_OBJ_BLOB) return 0;
        used = vcs_varint_read(header + 2, got - 2, &payload_len);
        if (!used) return 0;
        if ((long)payload_len > max_len || (long)payload_len < 0) return -1;
    }
    if (!vcsobjstore_read(st, oid, &type, data_out, len_out)) return 0;
    if (type != VCS_OBJ_BLOB) { free(*data_out); *data_out = NULL; return 0; }
    if (*len_out > max_len) { free(*data_out); *data_out = NULL; return -1; }
    return 1;
}
