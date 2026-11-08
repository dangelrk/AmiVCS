/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * diff.c -- see diff.h.
 *
 * Lines are first mapped to small integers (equal text = equal number,
 * via a hash table over both sides), so the search compares ints. The
 * search marks deleted lines of a and inserted lines of b in two flag
 * arrays (the way GNU diff does); the script is read off those flags at
 * the end, which keeps the recursion free of any output ordering.
 */
#include "diff.h"

#include <stdlib.h>
#include <string.h>

int vcs_diff_is_text(const void *data, long len)
{
    long probe = len < VCS_DIFF_TEXT_PROBE ? len : VCS_DIFF_TEXT_PROBE;
    if (len > VCS_DIFF_MAX_BYTES) return 0;
    if (probe > 0 && memchr(data, '\0', (size_t)probe)) return 0;
    return 1;
}

static unsigned long hash_bytes(const char *p, long n)
{
    unsigned long h = 5381;
    long i;
    for (i = 0; i < n; i++) h = (h * 33) ^ (unsigned char)p[i];
    return h;
}

int vcs_lines_split(const char *data, long len, VcsLines *out)
{
    long i, start = 0, n = 0, cap = 0;
    VcsLine *items = NULL;

    out->items = NULL;
    out->n = 0;
    for (i = 0; i < len; i++) if (data[i] == '\n' || data[i] == '\r') cap++;
    cap += 1;
    items = (VcsLine *)malloc(sizeof(VcsLine) * (size_t)cap);
    if (!items) return 0;

    i = 0;
    while (i < len) {
        long tend;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        tend = i;
        if (i < len) {
            if (data[i] == '\r' && i + 1 < len && data[i + 1] == '\n') i += 2;
            else i++;
        }
        items[n].p = data + start;
        items[n].len = i - start;
        items[n].tlen = tend - start;
        items[n].hash = hash_bytes(data + start, i - start);
        n++;
        start = i;
    }
    out->items = items;
    out->n = n;
    return 1;
}

void vcs_lines_free(VcsLines *l)
{
    free(l->items);
    l->items = NULL;
    l->n = 0;
}

int vcs_line_equal(const VcsLine *a, const VcsLine *b)
{
    return a->len == b->len && a->hash == b->hash && memcmp(a->p, b->p, (size_t)a->len) == 0;
}

void vcs_diff_script_free(VcsDiffScript *s)
{
    free(s->items);
    s->items = NULL;
    s->n = s->cap = 0;
}

static int script_add(VcsDiffScript *s, char op, long a, long b, long n)
{
    if (n <= 0) return 1;
    if (s->n > 0) {
        VcsDiffRun *last = &s->items[s->n - 1];
        if (last->op == op) { last->n += n; return 1; }
    }
    if (s->n == s->cap) {
        int newcap = s->cap ? s->cap * 2 : 16;
        VcsDiffRun *tmp = (VcsDiffRun *)realloc(s->items, sizeof(VcsDiffRun) * (size_t)newcap);
        if (!tmp) return 0;
        s->items = tmp;
        s->cap = newcap;
    }
    s->items[s->n].op = op;
    s->items[s->n].a = a;
    s->items[s->n].b = b;
    s->items[s->n].n = n;
    s->n++;
    return 1;
}

/* ---- line -> id ---------------------------------------------------- */

static int assign_ids(const VcsLines *a, const VcsLines *b, long *ida, long *idb)
{
    long total = a->n + b->n, size = 64, i, next = 0;
    long *slot;               /* hash table: index into `rep` + 1, 0 = empty */
    const VcsLine **rep;      /* representative line per id */

    while (size < total * 2) size <<= 1;
    slot = (long *)calloc((size_t)size, sizeof(long));
    rep = (const VcsLine **)malloc(sizeof(VcsLine *) * (size_t)(total ? total : 1));
    if (!slot || !rep) { free(slot); free(rep); return 0; }

    for (i = 0; i < total; i++) {
        const VcsLine *l = i < a->n ? &a->items[i] : &b->items[i - a->n];
        long h = (long)(l->hash & (unsigned long)(size - 1));
        long id;
        for (;;) {
            if (slot[h] == 0) { rep[next] = l; slot[h] = next + 1; id = next++; break; }
            if (vcs_line_equal(rep[slot[h] - 1], l)) { id = slot[h] - 1; break; }
            h = (h + 1) & (size - 1);
        }
        if (i < a->n) ida[i] = id; else idb[i - a->n] = id;
    }
    free(slot);
    free(rep);
    return 1;
}

/* ---- Myers, linear space ------------------------------------------- */

typedef struct {
    const long *a, *b;
    char *del, *ins;
    long *fv, *bv;   /* indexed by k + off */
    long off;
    long maxd;
} Ctx;

/* Finds a split point on an optimal path through a[x0..x1) x b[y0..y1)
 * (both non-empty, first and last elements differing). Returns 1 with
 * xm/ym set, 0 if more than maxd edits would be needed. */
static int middle_snake(Ctx *c, long x0, long x1, long y0, long y1, long *xm, long *ym)
{
    long N = x1 - x0, M = y1 - y0, delta = N - M;
    int odd = (int)(delta & 1);
    long dmax = (N + M + 1) / 2, d, k;
    long *fv = c->fv + c->off, *bv = c->bv + c->off;

    if (dmax > c->maxd) dmax = c->maxd;
    fv[1] = 0;
    bv[1] = 0;
    for (d = 0; d <= dmax; d++) {
        for (k = -d; k <= d; k += 2) {
            long x, y;
            x = (k == -d || (k != d && fv[k - 1] < fv[k + 1])) ? fv[k + 1] : fv[k - 1] + 1;
            y = x - k;
            while (x < N && y < M && c->a[x0 + x] == c->b[y0 + y]) { x++; y++; }
            fv[k] = x;
            if (odd && k >= delta - (d - 1) && k <= delta + (d - 1)) {
                if (fv[k] + bv[delta - k] >= N) { *xm = x0 + x; *ym = y0 + y; return 1; }
            }
        }
        for (k = -d; k <= d; k += 2) {
            long x, y;
            x = (k == -d || (k != d && bv[k - 1] < bv[k + 1])) ? bv[k + 1] : bv[k - 1] + 1;
            y = x - k;
            while (x < N && y < M && c->a[x1 - 1 - x] == c->b[y1 - 1 - y]) { x++; y++; }
            bv[k] = x;
            if (!odd && k >= delta - d && k <= delta + d) {
                if (bv[k] + fv[delta - k] >= N) { *xm = x1 - x; *ym = y1 - y; return 1; }
            }
        }
    }
    return 0;
}

static void compare(Ctx *c, long x0, long x1, long y0, long y1)
{
    for (;;) {
        long xm, ym, i;
        while (x0 < x1 && y0 < y1 && c->a[x0] == c->b[y0]) { x0++; y0++; }
        while (x0 < x1 && y0 < y1 && c->a[x1 - 1] == c->b[y1 - 1]) { x1--; y1--; }
        if (x0 == x1) { for (i = y0; i < y1; i++) c->ins[i] = 1; return; }
        if (y0 == y1) { for (i = x0; i < x1; i++) c->del[i] = 1; return; }
        if (!middle_snake(c, x0, x1, y0, y1, &xm, &ym)
            || (xm == x0 && ym == y0) || (xm == x1 && ym == y1)) {
            /* too different (or no progress): the region is replaced */
            for (i = x0; i < x1; i++) c->del[i] = 1;
            for (i = y0; i < y1; i++) c->ins[i] = 1;
            return;
        }
        compare(c, x0, xm, y0, ym);
        x0 = xm;   /* second half without recursing */
        y0 = ym;
    }
}

int vcs_diff_lines(const VcsLines *a, const VcsLines *b, VcsDiffScript *out)
{
    Ctx c;
    long *ida, *idb, vsize, i, j;
    int ok = 1;

    out->items = NULL;
    out->n = out->cap = 0;

    ida = (long *)malloc(sizeof(long) * (size_t)(a->n + 1));
    idb = (long *)malloc(sizeof(long) * (size_t)(b->n + 1));
    c.del = (char *)calloc((size_t)(a->n + 1), 1);
    c.ins = (char *)calloc((size_t)(b->n + 1), 1);
    c.maxd = (a->n + b->n + 1) / 2 + 1;
    if (c.maxd > VCS_DIFF_MAX_D) c.maxd = VCS_DIFF_MAX_D;
    c.off = c.maxd + 2;
    vsize = 2 * c.off + 1;
    c.fv = (long *)malloc(sizeof(long) * (size_t)vsize);
    c.bv = (long *)malloc(sizeof(long) * (size_t)vsize);
    if (!ida || !idb || !c.del || !c.ins || !c.fv || !c.bv || !assign_ids(a, b, ida, idb)) {
        ok = 0;
        goto done;
    }
    c.a = ida;
    c.b = idb;
    compare(&c, 0, a->n, 0, b->n);

    i = j = 0;
    while (ok && (i < a->n || j < b->n)) {
        if (i < a->n && c.del[i]) {
            long s = i;
            while (i < a->n && c.del[i]) i++;
            ok = script_add(out, '-', s, j, i - s);
        } else if (j < b->n && c.ins[j]) {
            long s = j;
            while (j < b->n && c.ins[j]) j++;
            ok = script_add(out, '+', i, s, j - s);
        } else {
            long si = i, sj = j;
            while (i < a->n && j < b->n && !c.del[i] && !c.ins[j]) { i++; j++; }
            ok = script_add(out, ' ', si, sj, i - si);
        }
    }

done:
    free(ida); free(idb); free(c.del); free(c.ins); free(c.fv); free(c.bv);
    if (!ok) vcs_diff_script_free(out);
    return ok;
}

/* ---- hunks ---------------------------------------------------------- */

static void emit_line(const VcsDiffSink *sink, void *ctx, char op, const VcsLine *l)
{
    sink->line(op, l->p, l->tlen, l->len > l->tlen, ctx);
}

int vcs_diff_emit(const VcsLines *a, const VcsLines *b, const VcsDiffScript *s,
                  int context, const VcsDiffSink *sink, void *ctx)
{
    int r = 0, hunks = 0;

    while (r < s->n) {
        int first, last, k;
        long lead = 0, trail = 0, acount = 0, bcount = 0, a0, b0, i;

        if (s->items[r].op == ' ') { r++; continue; }
        /* one hunk: change runs joined by equal runs of at most 2*context */
        first = last = r;
        for (k = r + 1; k < s->n; k++) {
            if (s->items[k].op != ' ') { last = k; continue; }
            if (k + 1 < s->n && s->items[k].n <= 2L * context) continue;
            break;
        }
        if (first > 0) {
            lead = s->items[first - 1].n;          /* an equal run */
            if (lead > context) lead = context;
        }
        if (last + 1 < s->n) {
            trail = s->items[last + 1].n;
            if (trail > context) trail = context;
        }
        acount = bcount = lead + trail;
        for (k = first; k <= last; k++) {
            const VcsDiffRun *run = &s->items[k];
            if (run->op != '+') acount += run->n;
            if (run->op != '-') bcount += run->n;
        }
        a0 = s->items[first].a - lead;
        b0 = s->items[first].b - lead;
        if (sink->hunk)
            sink->hunk(acount ? a0 + 1 : a0, acount, bcount ? b0 + 1 : b0, bcount, ctx);
        if (sink->line) {
            for (i = a0; i < a0 + lead; i++) emit_line(sink, ctx, ' ', &a->items[i]);
            for (k = first; k <= last; k++) {
                const VcsDiffRun *run = &s->items[k];
                if (run->op == '+')
                    for (i = run->b; i < run->b + run->n; i++) emit_line(sink, ctx, '+', &b->items[i]);
                else
                    for (i = run->a; i < run->a + run->n; i++) emit_line(sink, ctx, run->op, &a->items[i]);
            }
            if (trail > 0) {
                const VcsDiffRun *run = &s->items[last + 1];
                for (i = run->a; i < run->a + trail; i++) emit_line(sink, ctx, ' ', &a->items[i]);
            }
        }
        hunks++;
        r = last + 1;
    }
    return hunks;
}

int vcs_diff_buffers(const char *a, long alen, const char *b, long blen,
                     int context, const VcsDiffSink *sink, void *ctx)
{
    VcsLines la, lb;
    VcsDiffScript s;
    int n;

    if (!vcs_lines_split(a, alen, &la)) return -1;
    if (!vcs_lines_split(b, blen, &lb)) { vcs_lines_free(&la); return -1; }
    if (!vcs_diff_lines(&la, &lb, &s)) { vcs_lines_free(&la); vcs_lines_free(&lb); return -1; }
    n = vcs_diff_emit(&la, &lb, &s, context, sink, ctx);
    vcs_diff_script_free(&s);
    vcs_lines_free(&la);
    vcs_lines_free(&lb);
    return n;
}
