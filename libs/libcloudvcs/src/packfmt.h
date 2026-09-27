/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_PACKFMT_H
#define CLOUDVCS_PACKFMT_H

/*
 * packfmt.h -- private, shared pack on-disk format description
 * (plan section 9's "Hybrid": loose objects bundled into read-only packs
 * by gc). Used by BOTH objstore.c (transparent read fallback -- callers
 * of vcsobjstore_read()/vcsobjstore_extract_blob_to_file() never need to
 * know whether an object is loose or packed) and pack.c (the writer/gc
 * side) -- kept as one private module so the two sides can't drift apart
 * on the byte layout.
 *
 * <amivcs>/objects/pack/pack-<hex>.pack:
 *   [u8 format_version][varint count]
 *   count * { [u8 type][varint payload_len][payload] }
 *
 * <amivcs>/objects/pack/pack-<hex>.idx (same <hex>, sorted by oid so
 * vcs_pack_lookup() can binary-search it):
 *   [u8 format_version][varint count]
 *   count * { [16-byte oid][varint offset_in_pack][varint payload_len] }
 *
 * A pack's own <hex> name is the MD5 of the finished .pack file's bytes
 * -- reuses the same content-addressing idea as every other object here,
 * just applied to the bundle instead of a single blob/tree/commit.
 */

#include "object.h"
#include "objstore.h"

#define VCS_PACK_DIR "pack"

/* Looks up oid across every pack-*.idx under <store>/objects/pack/ (v1
 * doesn't merge/repack across gc runs -- plan section 6, avoid premature
 * generality -- so there may be several). On success returns 1 and fills
 * type_out, data_out (malloc'd, caller frees) and len_out. Returns 0 if
 * oid isn't in any pack -- the normal case when it's loose or doesn't exist;
 * objstore.c's read functions check loose objects first and only fall
 * back to this. */
int vcs_pack_lookup(const VcsObjStore *store, const VcsOid *oid,
                    VcsObjType *type_out, void **data_out, long *len_out);

#endif /* CLOUDVCS_PACKFMT_H */
