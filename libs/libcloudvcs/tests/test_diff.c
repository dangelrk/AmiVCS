/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * test_diff.c -- diff.c: the edit script must turn a into b, be minimal
 * (checked against a plain LCS table on small random texts), and the
 * hunk output must match the unified-diff conventions.
 */
#include "diff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* rebuild b from a and the script; count edits */
static int apply_ok(const VcsLines *a, const VcsLines *b, const VcsDiffScript *s, long *edits)
{
    long ai = 0, bi = 0;
    int r;
    *edits = 0;
    for (r = 0; r < s->n; r++) {
        const VcsDiffRun *run = &s->items[r];
        long i;
        if (run->op == ' ') {
            if (run->a != ai || run->b != bi) return 0;
            for (i = 0; i < run->n; i++)
                if (!vcs_line_equal(&a->items[ai + i], &b->items[bi + i])) return 0;
            ai += run->n; bi += run->n;
        } else if (run->op == '-') {
            if (run->a != ai) return 0;
            ai += run->n; *edits += run->n;
        } else {
            if (run->b != bi) return 0;
            bi += run->n; *edits += run->n;
        }
    }
    return ai == a->n && bi == b->n;
}

static long lcs(const VcsLines *a, const VcsLines *b)
{
    long *t = (long *)calloc((size_t)((a->n + 1) * (b->n + 1)), sizeof(long));
    long i, j, r;
    for (i = 1; i <= a->n; i++)
        for (j = 1; j <= b->n; j++) {
            long *c = &t[i * (b->n + 1) + j];
            if (vcs_line_equal(&a->items[i - 1], &b->items[j - 1])) *c = t[(i - 1) * (b->n + 1) + j - 1] + 1;
            else {
                long u = t[(i - 1) * (b->n + 1) + j], l = t[i * (b->n + 1) + j - 1];
                *c = u > l ? u : l;
            }
        }
    r = t[a->n * (b->n + 1) + b->n];
    free(t);
    return r;
}

static void random_text(char *buf, int lines, int alphabet)
{
    int i;
    buf[0] = '\0';
    for (i = 0; i < lines; i++) {
        char l[8];
        snprintf(l, sizeof(l), "%c\n", 'a' + rand() % alphabet);
        strcat(buf, l);
    }
}

/* ---- unified rendering for the fixed cases ---- */
static char out[8192];
static void h_cb(long as, long ac, long bs, long bc, void *ctx)
{
    char t[64];
    (void)ctx;
    snprintf(t, sizeof(t), "@@ -%ld,%ld +%ld,%ld @@\n", as, ac, bs, bc);
    strcat(out, t);
}
static void l_cb(char op, const char *text, long len, int eol, void *ctx)
{
    size_t n = strlen(out);
    (void)ctx;
    out[n++] = op;
    memcpy(out + n, text, (size_t)len);
    n += (size_t)len;
    out[n] = '\0';
    strcat(out, eol ? "\n" : "\n\\ No newline at end of file\n");
}
static const VcsDiffSink sink = { h_cb, l_cb };

static void unified(const char *a, const char *b)
{
    out[0] = '\0';
    vcs_diff_buffers(a, (long)strlen(a), b, (long)strlen(b), 3, &sink, NULL);
}

int main(void)
{
    int round;
    char a[4096], b[4096];

    srand(12345);
    for (round = 0; round < 3000; round++) {
        VcsLines la, lb;
        VcsDiffScript s;
        long edits, best;
        random_text(a, rand() % 40, 2 + rand() % 5);
        random_text(b, rand() % 40, 2 + rand() % 5);
        vcs_lines_split(a, (long)strlen(a), &la);
        vcs_lines_split(b, (long)strlen(b), &lb);
        CHECK(vcs_diff_lines(&la, &lb, &s), "diff failed");
        CHECK(apply_ok(&la, &lb, &s, &edits), "script does not turn a into b (round %d)", round);
        best = la.n + lb.n - 2 * lcs(&la, &lb);
        CHECK(edits == best, "not minimal: %ld edits, best %ld (round %d)", edits, best, round);
        vcs_diff_script_free(&s);
        vcs_lines_free(&la);
        vcs_lines_free(&lb);
    }

    /* line splitting: CRLF, lone CR, no final line end */
    {
        VcsLines l;
        vcs_lines_split("x\r\ny\rz", 6, &l);
        CHECK(l.n == 3 && l.items[0].len == 3 && l.items[0].tlen == 1
              && l.items[1].len == 2 && l.items[2].len == 1 && l.items[2].tlen == 1, "split");
        vcs_lines_free(&l);
        vcs_lines_split("", 0, &l);
        CHECK(l.n == 0, "empty split");
        vcs_lines_free(&l);
    }

    unified("a\nb\nc\n", "a\nB\nc\n");
    CHECK(strcmp(out, "@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n") == 0, "simple: %s", out);

    unified("", "x\ny\n");
    CHECK(strcmp(out, "@@ -0,0 +1,2 @@\n+x\n+y\n") == 0, "insert into empty: %s", out);

    unified("x\ny\n", "");
    CHECK(strcmp(out, "@@ -1,2 +0,0 @@\n-x\n-y\n") == 0, "delete all: %s", out);

    unified("a\nb", "a\nb\n");
    CHECK(strcmp(out, "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n") == 0, "eol: %s", out);

    unified("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n", "1\nX\n3\n4\n5\n6\n7\n8\n9\n10\nY\n12\n");
    CHECK(strcmp(out, "@@ -1,5 +1,5 @@\n 1\n-2\n+X\n 3\n 4\n 5\n@@ -8,5 +8,5 @@\n 8\n 9\n 10\n-11\n+Y\n 12\n") == 0,
          "two hunks: %s", out);

    unified("1\n2\n3\n4\n5\n6\n7\n8\n", "1\nX\n3\n4\n5\n6\nY\n8\n");
    CHECK(strcmp(out, "@@ -1,8 +1,8 @@\n 1\n-2\n+X\n 3\n 4\n 5\n 6\n-7\n+Y\n 8\n") == 0, "joined hunk: %s", out);

    unified("same\n", "same\n");
    CHECK(out[0] == '\0', "no change: %s", out);

    CHECK(vcs_diff_is_text("abc", 3), "text");
    CHECK(!vcs_diff_is_text("a\0c", 3), "binary");

    /* the edit limit: two big unrelated texts still yield a valid script */
    {
        static char x[200000], y[200000];
        VcsLines la, lb;
        VcsDiffScript s;
        long edits, i;
        size_t px = 0, py = 0;
        for (i = 0; i < 6000; i++) {
            px += (size_t)snprintf(x + px, sizeof(x) - px, "x%ld\n", i);
            py += (size_t)snprintf(y + py, sizeof(y) - py, "y%ld\n", i);
        }
        vcs_lines_split(x, (long)px, &la);
        vcs_lines_split(y, (long)py, &lb);
        CHECK(vcs_diff_lines(&la, &lb, &s), "big diff failed");
        CHECK(apply_ok(&la, &lb, &s, &edits) && edits == 12000, "big diff script");
        vcs_diff_script_free(&s);
        vcs_lines_free(&la);
        vcs_lines_free(&lb);
    }

    if (fails) { printf("test_diff: %d failure(s)\n", fails); return 1; }
    printf("test_diff: ok\n");
    return 0;
}
