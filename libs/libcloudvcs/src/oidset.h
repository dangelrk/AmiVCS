/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_OIDSET_H
#define CLOUDVCS_OIDSET_H

/*
 * oidset.h -- a small OID set (dynamic array + linear membership check),
 * shared by merge.c (commit-ancestor walks) and remote.c (object-graph
 * walks for push/pull). Repo histories/object graphs at Amiga scale are
 * small enough that this needs no hash table.
 */

#include "object.h"

typedef struct { VcsOid *items; int n, cap; } VcsOidSet;

void vcs_oidset_init(VcsOidSet *s);
void vcs_oidset_free(VcsOidSet *s);
int  vcs_oidset_contains(const VcsOidSet *s, const VcsOid *oid);
void vcs_oidset_add(VcsOidSet *s, const VcsOid *oid);

#endif /* CLOUDVCS_OIDSET_H */
