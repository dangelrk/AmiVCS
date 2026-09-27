/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_PACK_H
#define CLOUDVCS_PACK_H

/*
 * pack.h -- gc: bundles loose objects into a pack (plan section 9's
 * "Hybrid" storage -- loose while writing, packed on demand). Amiga FFS
 * pays per directory entry, so turning thousands of tiny loose files
 * into one pack + a compact index is the whole point of running this
 * periodically on a long-lived repository.
 *
 * v1 doesn't merge or repack existing packs (plan section 6: avoid
 * premature generality) -- every gc run bundles whatever is currently
 * loose into one new pack and removes those loose copies; previously
 * packed objects, and any older packs, are left exactly as they are.
 * Nor is anything pruned for unreachability -- gc here only changes HOW
 * objects are stored, never WHICH ones exist.
 */

#include "objstore.h"

/* Packs every currently-loose object in store into a new pack under
 * <store>/objects/pack/ and removes the loose copies that made it in.
 * out_count, if non-NULL, receives the number of objects packed (0 if
 * there was nothing loose to do). Returns 1 on success, 0 on I/O error --
 * partial failures leave the offending objects loose rather than losing
 * them (a loose object is only pruned after its pack+index are safely on
 * disk under their final names). */
int vcs_pack_gc(VcsObjStore *store, int *out_count);

#endif /* CLOUDVCS_PACK_H */
