/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "oidset.h"

#include <stdlib.h>

void vcs_oidset_init(VcsOidSet *s) { s->items = NULL; s->n = s->cap = 0; }

void vcs_oidset_free(VcsOidSet *s)
{
    free(s->items);
    s->items = NULL;
    s->n = s->cap = 0;
}

int vcs_oidset_contains(const VcsOidSet *s, const VcsOid *oid)
{
    int i;
    for (i = 0; i < s->n; i++) if (vcs_oid_equal(&s->items[i], oid)) return 1;
    return 0;
}

void vcs_oidset_add(VcsOidSet *s, const VcsOid *oid)
{
    if (s->n == s->cap) {
        int newcap = s->cap ? s->cap * 2 : 16;
        VcsOid *tmp = (VcsOid *)realloc(s->items, sizeof(VcsOid) * (size_t)newcap);
        if (!tmp) return;
        s->items = tmp;
        s->cap = newcap;
    }
    s->items[s->n++] = *oid;
}
