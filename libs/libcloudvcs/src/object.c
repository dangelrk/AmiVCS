/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * object.c -- see object.h. Encoding is written by hand into a single
 * upper-bound-sized buffer (no growable-buffer abstraction -- these
 * payloads are tiny: a handful of tree entries or one commit's fields,
 * never a whole file, so there is nothing to amortize) and then
 * realloc()'d down to the bytes actually used before returning, so the
 * caller doesn't hold onto slack RAM.
 */
#include "object.h"
#include "varint.h"
#include "md5.h"

#include <stdlib.h>
#include <string.h>

const VcsOid VCS_OID_ZERO = { { 0 } };

int vcs_oid_equal(const VcsOid *a, const VcsOid *b)
{
    return memcmp(a->b, b->b, VCS_OID_SIZE) == 0;
}

int vcs_oid_is_zero(const VcsOid *o)
{
    return vcs_oid_equal(o, &VCS_OID_ZERO);
}

void vcs_oid_to_hex(const VcsOid *o, char hex[VCS_OID_HEX])
{
    md5_to_hex(o->b, hex);
}

static int hexval(char c, unsigned char *out)
{
    if (c >= '0' && c <= '9') { *out = (unsigned char)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *out = (unsigned char)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *out = (unsigned char)(c - 'A' + 10); return 1; }
    return 0;
}

int vcs_oid_from_hex(const char *hex, VcsOid *out)
{
    VcsOid tmp;
    int i;
    if (strlen(hex) != (size_t)(VCS_OID_HEX - 1)) return 0;
    for (i = 0; i < VCS_OID_SIZE; i++) {
        unsigned char hi, lo;
        if (!hexval(hex[i * 2], &hi) || !hexval(hex[i * 2 + 1], &lo)) return 0;
        tmp.b[i] = (unsigned char)((hi << 4) | lo);
    }
    *out = tmp;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Tree                                                                  */

void vcs_tree_init(VcsTree *t)
{
    t->items = NULL;
    t->n = 0;
    t->cap = 0;
}

void vcs_tree_free(VcsTree *t)
{
    free(t->items);
    t->items = NULL;
    t->n = t->cap = 0;
}

static int tree_find_index(const VcsTree *t, const char *name, int *found)
{
    /* Plain binary search -- items[] is kept sorted by vcs_tree_upsert().
     * Returns the index of the match (*found=1) or the insertion point
     * that keeps the array sorted (*found=0). */
    int lo = 0, hi = t->n;
    *found = 0;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(t->items[mid].name, name);
        if (cmp == 0) { *found = 1; return mid; }
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

void vcs_tree_upsert(VcsTree *t, const char *name, int is_dir, const VcsOid *oid)
{
    int found;
    int idx = tree_find_index(t, name, &found);

    if (found) {
        t->items[idx].is_dir = is_dir;
        t->items[idx].oid = *oid;
        return;
    }

    if (t->n == t->cap) {
        int newcap = t->cap ? t->cap * 2 : 16;
        VcsTreeEntry *tmp = (VcsTreeEntry *)realloc(t->items, sizeof(VcsTreeEntry) * (size_t)newcap);
        if (!tmp) return;
        t->items = tmp;
        t->cap = newcap;
    }
    if (idx < t->n) {
        memmove(&t->items[idx + 1], &t->items[idx], sizeof(VcsTreeEntry) * (size_t)(t->n - idx));
    }
    strncpy(t->items[idx].name, name, sizeof(t->items[idx].name) - 1);
    t->items[idx].name[sizeof(t->items[idx].name) - 1] = '\0';
    t->items[idx].is_dir = is_dir;
    t->items[idx].oid = *oid;
    t->n++;
}

const VcsTreeEntry *vcs_tree_find(const VcsTree *t, const char *name)
{
    int found;
    int idx = tree_find_index(t, name, &found);
    return found ? &t->items[idx] : NULL;
}

int vcs_tree_encode(const VcsTree *t, void **out_data, long *out_len)
{
    size_t cap, pos, i;
    unsigned char *buf;

    cap = 1 /* version */ + 10 /* count varint */;
    for (i = 0; i < (size_t)t->n; i++) {
        cap += 1 /* type */ + 1 /* namelen */ + strlen(t->items[i].name) + VCS_OID_SIZE;
    }

    buf = (unsigned char *)malloc(cap ? cap : 1);
    if (!buf) return 0;

    pos = 0;
    buf[pos++] = VCS_FORMAT_VERSION;
    pos += vcs_varint_write(buf + pos, (unsigned long)t->n);

    for (i = 0; i < (size_t)t->n; i++) {
        const VcsTreeEntry *e = &t->items[i];
        size_t namelen = strlen(e->name);
        if (namelen > 255) namelen = 255; /* name[] is already capped at this size */
        buf[pos++] = e->is_dir ? 1 : 0;
        buf[pos++] = (unsigned char)namelen;
        memcpy(buf + pos, e->name, namelen);
        pos += namelen;
        memcpy(buf + pos, e->oid.b, VCS_OID_SIZE);
        pos += VCS_OID_SIZE;
    }

    {
        unsigned char *shrunk = (unsigned char *)realloc(buf, pos ? pos : 1);
        if (shrunk) buf = shrunk;
    }
    *out_data = buf;
    *out_len = (long)pos;
    return 1;
}

int vcs_tree_decode(const void *data, long len, VcsTree *t)
{
    const unsigned char *buf = (const unsigned char *)data;
    size_t pos = 0, remaining = (size_t)len;
    unsigned long count, i;
    size_t used;

    vcs_tree_init(t);
    if (len < 1) return 0;
    if (buf[pos] != VCS_FORMAT_VERSION) return 0;
    pos++; remaining--;

    used = vcs_varint_read(buf + pos, remaining, &count);
    if (!used) return 0;
    pos += used; remaining -= used;

    for (i = 0; i < count; i++) {
        int is_dir;
        unsigned char namelen;
        char name[256];
        VcsOid oid;

        if (remaining < 2) { vcs_tree_free(t); return 0; }
        is_dir = buf[pos] ? 1 : 0; pos++; remaining--;
        namelen = buf[pos]; pos++; remaining--;

        if (remaining < (size_t)namelen + VCS_OID_SIZE) { vcs_tree_free(t); return 0; }
        memcpy(name, buf + pos, namelen);
        name[namelen] = '\0';
        pos += namelen; remaining -= namelen;

        memcpy(oid.b, buf + pos, VCS_OID_SIZE);
        pos += VCS_OID_SIZE; remaining -= VCS_OID_SIZE;

        vcs_tree_upsert(t, name, is_dir, &oid);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Commit                                                                */

void vcs_commit_init(VcsCommit *c)
{
    memset(c, 0, sizeof(*c));
    c->tree = VCS_OID_ZERO;
}

void vcs_commit_free(VcsCommit *c)
{
    free(c->message);
    c->message = NULL;
}

int vcs_commit_encode(const VcsCommit *c, void **out_data, long *out_len)
{
    size_t cap, pos, authorlen, msglen, i;
    unsigned char *buf;

    authorlen = strlen(c->author);
    if (authorlen > 255) authorlen = 255;
    msglen = c->message ? strlen(c->message) : 0;

    cap = 1 /* version */ + VCS_OID_SIZE /* tree */ + 1 /* nparents */
        + (size_t)c->nparents * VCS_OID_SIZE
        + 10 /* mtime varint */
        + 1 + authorlen
        + 10 /* msglen varint */ + msglen;

    buf = (unsigned char *)malloc(cap ? cap : 1);
    if (!buf) return 0;

    pos = 0;
    buf[pos++] = VCS_FORMAT_VERSION;
    memcpy(buf + pos, c->tree.b, VCS_OID_SIZE);
    pos += VCS_OID_SIZE;

    buf[pos++] = (unsigned char)c->nparents;
    for (i = 0; i < (size_t)c->nparents && i < VCS_MAX_PARENTS; i++) {
        memcpy(buf + pos, c->parents[i].b, VCS_OID_SIZE);
        pos += VCS_OID_SIZE;
    }

    pos += vcs_varint_write(buf + pos, (unsigned long)c->mtime);

    buf[pos++] = (unsigned char)authorlen;
    memcpy(buf + pos, c->author, authorlen);
    pos += authorlen;

    pos += vcs_varint_write(buf + pos, (unsigned long)msglen);
    if (msglen) {
        memcpy(buf + pos, c->message, msglen);
        pos += msglen;
    }

    {
        unsigned char *shrunk = (unsigned char *)realloc(buf, pos ? pos : 1);
        if (shrunk) buf = shrunk;
    }
    *out_data = buf;
    *out_len = (long)pos;
    return 1;
}

int vcs_commit_decode(const void *data, long len, VcsCommit *c)
{
    const unsigned char *buf = (const unsigned char *)data;
    size_t pos = 0, remaining = (size_t)len;
    unsigned long mtime_u, msglen_u;
    unsigned char authorlen;
    size_t used, i;

    vcs_commit_init(c);
    if (len < 1) return 0;
    if (buf[pos] != VCS_FORMAT_VERSION) return 0;
    pos++; remaining--;

    if (remaining < VCS_OID_SIZE) return 0;
    memcpy(c->tree.b, buf + pos, VCS_OID_SIZE);
    pos += VCS_OID_SIZE; remaining -= VCS_OID_SIZE;

    if (remaining < 1) return 0;
    c->nparents = buf[pos]; pos++; remaining--;
    if (c->nparents < 0 || c->nparents > VCS_MAX_PARENTS) return 0;

    for (i = 0; i < (size_t)c->nparents; i++) {
        if (remaining < VCS_OID_SIZE) return 0;
        memcpy(c->parents[i].b, buf + pos, VCS_OID_SIZE);
        pos += VCS_OID_SIZE; remaining -= VCS_OID_SIZE;
    }

    used = vcs_varint_read(buf + pos, remaining, &mtime_u);
    if (!used) return 0;
    pos += used; remaining -= used;
    c->mtime = (long)mtime_u;

    if (remaining < 1) return 0;
    authorlen = buf[pos]; pos++; remaining--;
    if (remaining < authorlen || authorlen >= sizeof(c->author)) return 0;
    memcpy(c->author, buf + pos, authorlen);
    c->author[authorlen] = '\0';
    pos += authorlen; remaining -= authorlen;

    used = vcs_varint_read(buf + pos, remaining, &msglen_u);
    if (!used) return 0;
    pos += used; remaining -= used;
    if (remaining < msglen_u) return 0;

    c->message = (char *)malloc((size_t)msglen_u + 1);
    if (!c->message) return 0;
    memcpy(c->message, buf + pos, msglen_u);
    c->message[msglen_u] = '\0';

    return 1;
}
