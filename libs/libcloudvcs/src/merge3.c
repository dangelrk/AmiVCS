/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * merge3.c -- see merge3.h.
 */
#include "merge3.h"
#include "diff.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;
    long  n, cap;
    int   oom;
} Buf;

static void put(Buf *b, const char *s, long n)
{
    if (b->oom || n <= 0) return;
    if (b->n + n > b->cap) {
        long cap = b->cap ? b->cap : 1024;
        char *t;
        while (cap < b->n + n) cap *= 2;
        t = (char *)realloc(b->p, (size_t)cap);
        if (!t) { b->oom = 1; return; }
        b->p = t;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
}

static void put_lines(Buf *b, const VcsLines *l, long from, long to)
{
    long i;
    for (i = from; i < to; i++) put(b, l->items[i].p, l->items[i].len);
}

/* inside a conflict every line needs an end, also a file's last one */
static void put_lines_eol(Buf *b, const VcsLines *l, long from, long to, const char *eol)
{
    long i;
    for (i = from; i < to; i++) {
        put(b, l->items[i].p, l->items[i].len);
        if (l->items[i].len == l->items[i].tlen) put(b, eol, (long)strlen(eol));
    }
}

static void put_marker(Buf *b, const char *m, const char *label, const char *eol)
{
    put(b, m, (long)strlen(m));
    if (label && label[0]) { put(b, " ", 1); put(b, label, (long)strlen(label)); }
    put(b, eol, (long)strlen(eol));
}

static int range_equal(const VcsLines *x, long x0, long x1, const VcsLines *y, long y0, long y1)
{
    long i;
    if (x1 - x0 != y1 - y0) return 0;
    for (i = 0; i < x1 - x0; i++)
        if (!vcs_line_equal(&x->items[x0 + i], &y->items[y0 + i])) return 0;
    return 1;
}

/* match[i] = index in the other text of base line i, -1 if it changed */
static int base_matches(const VcsLines *base, const VcsLines *other, long *match)
{
    VcsDiffScript s;
    int r;
    long i;
    if (!vcs_diff_lines(base, other, &s)) return 0;
    for (i = 0; i < base->n; i++) match[i] = -1;
    for (r = 0; r < s.n; r++)
        if (s.items[r].op == ' ')
            for (i = 0; i < s.items[r].n; i++) match[s.items[r].a + i] = s.items[r].b + i;
    vcs_diff_script_free(&s);
    return 1;
}

static const char *line_end_of(const VcsLines *l)
{
    long i;
    for (i = 0; i < l->n; i++) {
        const VcsLine *x = &l->items[i];
        if (x->len > x->tlen) {
            if (x->len - x->tlen == 2) return "\r\n";
            return x->p[x->tlen] == '\r' ? "\r" : "\n";
        }
    }
    return NULL;
}

int vcs_merge3(const char *base, long blen, const char *ours, long olen,
               const char *theirs, long tlen,
               const char *ours_label, const char *theirs_label,
               char **out, long *outlen, int *nconflicts)
{
    VcsLines lb, lo, lt;
    long *mo = NULL, *mt = NULL;
    long bi = 0, oi = 0, ti = 0;
    const char *eol;
    Buf b;
    int ok = 0;

    *out = NULL;
    *outlen = 0;
    *nconflicts = 0;
    memset(&b, 0, sizeof(b));
    lb.items = lo.items = lt.items = NULL;
    lb.n = lo.n = lt.n = 0;

    if (!vcs_lines_split(base, blen, &lb) || !vcs_lines_split(ours, olen, &lo)
        || !vcs_lines_split(theirs, tlen, &lt))
        goto done;
    mo = (long *)malloc(sizeof(long) * (size_t)(lb.n + 1));
    mt = (long *)malloc(sizeof(long) * (size_t)(lb.n + 1));
    if (!mo || !mt || !base_matches(&lb, &lo, mo) || !base_matches(&lb, &lt, mt)) goto done;

    eol = line_end_of(&lo);
    if (!eol) eol = line_end_of(&lt);
    if (!eol) eol = line_end_of(&lb);
    if (!eol) eol = "\n";

    for (;;) {
        long j, oe, te;
        if (bi < lb.n && mo[bi] == oi && mt[bi] == ti) {   /* kept by both */
            put(&b, lb.items[bi].p, lb.items[bi].len);
            bi++; oi++; ti++;
            continue;
        }
        if (bi >= lb.n && oi >= lo.n && ti >= lt.n) break;

        /* the chunk up to the next base line both sides kept */
        j = bi;
        while (j < lb.n && (mo[j] < 0 || mt[j] < 0)) j++;
        oe = j < lb.n ? mo[j] : lo.n;
        te = j < lb.n ? mt[j] : lt.n;

        {
            int ours_changed = !range_equal(&lo, oi, oe, &lb, bi, j);
            int theirs_changed = !range_equal(&lt, ti, te, &lb, bi, j);
            if (!ours_changed) {
                put_lines(&b, &lt, ti, te);
            } else if (!theirs_changed || range_equal(&lo, oi, oe, &lt, ti, te)) {
                put_lines(&b, &lo, oi, oe);
            } else {
                /* conflict; lines both sides share at its edges stay outside */
                long pre = 0, suf = 0;
                while (oi + pre < oe && ti + pre < te
                       && vcs_line_equal(&lo.items[oi + pre], &lt.items[ti + pre])) pre++;
                while (oe - suf > oi + pre && te - suf > ti + pre
                       && vcs_line_equal(&lo.items[oe - 1 - suf], &lt.items[te - 1 - suf])) suf++;
                put_lines_eol(&b, &lo, oi, oi + pre, eol);
                put_marker(&b, "<<<<<<<", ours_label, eol);
                put_lines_eol(&b, &lo, oi + pre, oe - suf, eol);
                put_marker(&b, "=======", NULL, eol);
                put_lines_eol(&b, &lt, ti + pre, te - suf, eol);
                put_marker(&b, ">>>>>>>", theirs_label, eol);
                put_lines(&b, &lo, oe - suf, oe);
                (*nconflicts)++;
            }
        }
        bi = j; oi = oe; ti = te;
    }
    if (!b.oom) ok = 1;

done:
    free(mo);
    free(mt);
    vcs_lines_free(&lb);
    vcs_lines_free(&lo);
    vcs_lines_free(&lt);
    if (!ok) { free(b.p); return 0; }
    if (!b.p) { b.p = (char *)malloc(1); if (!b.p) return 0; }
    *out = b.p;
    *outlen = b.n;
    return 1;
}
