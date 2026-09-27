/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_OBJSTORE_H
#define CLOUDVCS_OBJSTORE_H

/*
 * objstore.h -- the local, hybrid object database (docs/design/
 * 08_VCS_Grundkonzept.md's Blob/Tree/Commit storage; plan section 9).
 *
 * Loose objects live at <root>/objects/<2-hex>/<30-hex>, fan-out on the
 * first byte of the OID so a single directory never holds more than ~256
 * entries -- FFS pays per directory entry, so this keeps lookups and
 * directory scans cheap even with thousands of objects (plan section 3,
 * "Amiga-optimierte Datenstrukturen"). `gc` (see pack.h) later bundles
 * loose objects into read-only packs and prunes the loose copies; this
 * module's read path always checks loose objects first, then falls back
 * to any pack indices registered by pack.h, so callers never need to know
 * which form an object is currently stored in.
 *
 * One loose file = [u8 format_version][u8 type][varint payload_len]
 * [payload]. The OID hashes ONLY the payload (object.h) -- the header is
 * store plumbing, not part of an object's identity.
 */

#include "object.h"

#define VCS_DIR_NAME ".amivcs"

typedef struct {
    char root[1024]; /* absolute or cwd-relative path to <repo>/.amivcs */
} VcsObjStore;

/* amivcs_dir is the full path to the repo's .amivcs directory (already
 * resolved by repo.h) -- objstore itself has no opinion on where that is. */
void vcsobjstore_init(VcsObjStore *st, const char *amivcs_dir);

/* Creates <root>/objects (and objects/pack, for pack.h). Returns 1 on
 * success. Called once by repo_init(); harmless to call again. */
int vcsobjstore_create_dirs(const VcsObjStore *st);

int vcsobjstore_exists(const VcsObjStore *st, const VcsOid *oid);

/* Writes an in-RAM payload (trees, commits, and small blobs) as a loose
 * object. A no-op beyond computing the OID if the object already exists
 * (content-addressed dedup, plan section 3). Returns 1 on success. */
int vcsobjstore_write(VcsObjStore *st, VcsObjType type, const void *data, long len, VcsOid *out_oid);

/* Streams local_path's content in as a VCS_OBJ_BLOB without ever holding
 * the whole file in RAM (plan section 3/9 -- important on an 8 MB Amiga).
 * Returns 1 on success, 0 if local_path can't be read. */
int vcsobjstore_write_blob_file(VcsObjStore *st, const char *local_path, VcsOid *out_oid);

/* Reads an object fully into RAM -- fine for trees/commits (always small);
 * callers wanting a blob's bytes on disk should use the streaming
 * extractor below instead. *data_out is malloc'd, caller frees. Returns 1
 * on success, 0 if oid is not found. */
int vcsobjstore_read(const VcsObjStore *st, const VcsOid *oid, VcsObjType *type_out, void **data_out, long *len_out);

/* Streams a VCS_OBJ_BLOB object's payload straight to local_path (checkout)
 * without buffering it whole in RAM. Returns 1 on success, 0 if oid is
 * missing, not a blob, or local_path can't be created. */
int vcsobjstore_extract_blob_to_file(const VcsObjStore *st, const VcsOid *oid, const char *local_path);

#endif /* CLOUDVCS_OBJSTORE_H */
