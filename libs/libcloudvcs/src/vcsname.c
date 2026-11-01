/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * vcsname.c -- see vcsname.h.
 *
 * Two hash tables per operation:
 *   fwd  local relative path -> repository path, for every path whose
 *        local form does not convert back to it by the plain rule
 *        (learned from trees, loaded from .amivcs/localnames);
 *   rev  repository path -> local spelling actually found on disk, for
 *        a local name the plain rule would not reproduce (PC: a
 *        decomposed name from macOS, ...); filled by the scan.
 */
#include "vcsname.h"
#include "charset.h"
#include "localfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB        512           /* hash buckets */
#define NAMEBUF   1024          /* one path, local or repository */
#define CHUNK     300           /* text conversion piece (charset.c buffers are 1 KB) */
#define LOCALNAMES "localnames"

/* ------------------------------------------------------------------ */
/* small string map                                                    */

typedef struct Node { struct Node *next; char *key; char *val; } Node;
typedef struct { Node *b[NB]; } Map;

static Map g_fwd, g_rev;

static unsigned hash_str(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33u + (unsigned char)*s++;
    return h % NB;
}

static const char *map_get(const Map *m, const char *key)
{
    const Node *n;
    for (n = m->b[hash_str(key)]; n; n = n->next)
        if (strcmp(n->key, key) == 0) return n->val;
    return NULL;
}

static void map_put(Map *m, const char *key, const char *val)
{
    unsigned h = hash_str(key);
    Node *n;
    size_t kl, vl;
    for (n = m->b[h]; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            char *v;
            if (strcmp(n->val, val) == 0) return;
            v = (char *)malloc(strlen(val) + 1);
            if (!v) return;
            strcpy(v, val);
            free(n->val);
            n->val = v;
            return;
        }
    }
    kl = strlen(key) + 1;
    vl = strlen(val) + 1;
    n = (Node *)malloc(sizeof(Node));
    if (!n) return;
    n->key = (char *)malloc(kl);
    n->val = (char *)malloc(vl);
    if (!n->key || !n->val) { free(n->key); free(n->val); free(n); return; }
    memcpy(n->key, key, kl);
    memcpy(n->val, val, vl);
    n->next = m->b[h];
    m->b[h] = n;
}

static void map_del(Map *m, const char *key)
{
    Node **pp = &m->b[hash_str(key)];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            Node *n = *pp;
            *pp = n->next;
            free(n->key); free(n->val); free(n);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void map_clear(Map *m)
{
    int i;
    for (i = 0; i < NB; i++) {
        Node *n = m->b[i];
        while (n) { Node *x = n->next; free(n->key); free(n->val); free(n); n = x; }
        m->b[i] = NULL;
    }
}

void vcs_names_reset(void)
{
    map_clear(&g_fwd);
    map_clear(&g_rev);
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                       */

int vcs_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n, i;
        unsigned long cp;
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xE0) == 0xC0)      { n = 1; cp = *p & 0x1F; }
        else if ((*p & 0xF0) == 0xE0) { n = 2; cp = *p & 0x0F; }
        else if ((*p & 0xF8) == 0xF0) { n = 3; cp = *p & 0x07; }
        else return 0;
        for (i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)
            || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        p += n + 1;
    }
    return 1;
}

void vcs_utf8_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n;
    if (!dstsz) return;
    n = strlen(src ? src : "");
    if (n >= dstsz) {
        n = dstsz - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ISO-8859-1 -> UTF-8; out must hold 2 * strlen(in) + 1 */
static void latin1_to_utf8(const char *in, char *out)
{
    const unsigned char *p = (const unsigned char *)in;
    unsigned char *o = (unsigned char *)out;
    for (; *p; p++) {
        if (*p < 0x80) *o++ = *p;
        else { *o++ = (unsigned char)(0xC0 | (*p >> 6)); *o++ = (unsigned char)(0x80 | (*p & 0x3F)); }
    }
    *o = '\0';
}

/* the local side is UTF-8 (PC) */
static int local_is_utf8(void)
{
#ifdef PLATFORM_AMIGA
    return 0;              /* even a UTF-8 codeset in codesets.library: Amiga rules */
#else
    return strcmp(cs_name(), "UTF-8") == 0;
#endif
}

/* ------------------------------------------------------------------ */
/* one name component, plain rule                                      */

/* local -> repository */
static int plain_from_local(const char *l, char *out, size_t sz)
{
    if (local_is_utf8()) {
        if (!vcs_utf8_valid(l)) {          /* taken as it is */
            if (strlen(l) >= sz) return 0;
            strcpy(out, l);
            return 1;
        }
        return cs_nfc(l, out, sz);
    }
    return cs_to_utf8(l, out, sz);
}

/* repository -> local */
static int plain_to_local(const char *r, char *out, size_t sz)
{
    if (!vcs_utf8_valid(r)) {
        if (local_is_utf8()) {             /* PC: an older Amiga name, ISO-8859-1 */
            static char u[NAMEBUF * 2 + 1];
            if (strlen(r) >= NAMEBUF) return 0;
            latin1_to_utf8(r, u);
            cs_from_utf8(u, out, sz);
            return out[0] != '\0' || !r[0];
        }
        if (strlen(r) >= sz) return 0;     /* Amiga: already the local bytes */
        strcpy(out, r);
        return 1;
    }
    cs_from_utf8(r, out, sz);
    return out[0] != '\0' || !r[0];
}

/* 1 if plain_from_local(local) gives repo */
static int plain_roundtrip(const char *local, const char *repo)
{
    static char back[NAMEBUF * 3];
    return plain_from_local(local, back, sizeof(back)) && strcmp(back, repo) == 0;
}

/* appends "/comp" (or comp) to buf of size sz holding len bytes */
static int append_comp(char *buf, size_t sz, size_t *len, const char *comp)
{
    size_t cl = strlen(comp);
    if (*len + cl + 2 > sz) return 0;
    if (*len) buf[(*len)++] = '/';
    memcpy(buf + *len, comp, cl + 1);
    *len += cl;
    return 1;
}

/* next component of *p into comp (NAMEBUF); 0 at the end */
static int next_comp(const char **p, char *comp)
{
    const char *s = *p, *slash;
    size_t cl;
    if (!*s) return 0;
    slash = strchr(s, '/');
    cl = slash ? (size_t)(slash - s) : strlen(s);
    if (cl >= NAMEBUF) cl = NAMEBUF - 1;
    memcpy(comp, s, cl);
    comp[cl] = '\0';
    *p = slash ? slash + 1 : s + strlen(s);
    return 1;
}

/* ------------------------------------------------------------------ */
/* paths                                                               */

/* repo path -> local, optionally learning fwd pairs; see header */
static int to_local(const char *repo_path, char *out, size_t outsz, int learn)
{
    static char rp[NAMEBUF], lp[NAMEBUF], comp_r[NAMEBUF], comp_l[NAMEBUF];
    const char *p = repo_path;
    size_t rlen = 0, llen = 0;
    int exact = 1, ok = 1;

    if (!outsz) return -1;
    rp[0] = lp[0] = '\0';
    while (next_comp(&p, comp_r)) {
        const char *seen;
        int comp_exact;
        if (!append_comp(rp, sizeof(rp), &rlen, comp_r)) { ok = 0; break; }
        seen = map_get(&g_rev, rp);
        if (seen) {                         /* the spelling found on disk */
            size_t sl = strlen(seen);
            if (sl >= sizeof(lp)) { ok = 0; break; }
            memcpy(lp, seen, sl + 1);
            llen = sl;
            continue;
        }
        if (!plain_to_local(comp_r, comp_l, sizeof(comp_l))) { ok = 0; break; }
        {
            size_t parent_len = llen;
            if (!append_comp(lp, sizeof(lp), &llen, comp_l)) { ok = 0; break; }
            comp_exact = plain_roundtrip(comp_l, comp_r);
            /* PC: a valid but not composed name (an older build wrote the
             * NFD bytes macOS handed it) may still be on disk that way --
             * that spelling maps to it too */
            if (learn && !comp_exact && local_is_utf8() && vcs_utf8_valid(comp_r)) {
                static char alt[NAMEBUF];
                size_t al = parent_len;
                memcpy(alt, lp, parent_len);
                alt[al] = '\0';
                if (append_comp(alt, sizeof(alt), &al, comp_r)) map_put(&g_fwd, alt, rp);
            }
        }
        if (!comp_exact) exact = 0;
        if (learn) {
            /* only a component that needs it gets a pair (below it,
             * vcs_path_from_local() appends to the mapped parent); an
             * exact name overrides an older pair */
            if (!comp_exact) map_put(&g_fwd, lp, rp);
            else {
                const char *old = map_get(&g_fwd, lp);
                if (old && strcmp(old, rp) != 0) map_del(&g_fwd, lp);
            }
        }
    }
    if (!ok || llen >= outsz) { out[0] = '\0'; return -1; }
    memcpy(out, lp, llen + 1);
    return exact;
}

int vcs_path_to_local(const char *repo_path, char *out, size_t outsz)
{
    return to_local(repo_path, out, outsz, 0);
}

void vcs_names_learn(const char *repo_path)
{
    static char tmp[NAMEBUF];
    to_local(repo_path, tmp, sizeof(tmp), 1);
}

int vcs_path_from_local(const char *local_path, char *out, size_t outsz)
{
    static char lp[NAMEBUF], rp[NAMEBUF], comp_l[NAMEBUF], comp_r[NAMEBUF * 3], back[NAMEBUF];
    const char *p = local_path;
    size_t llen = 0, rlen = 0;

    if (!outsz) return 0;
    lp[0] = rp[0] = '\0';
    while (next_comp(&p, comp_l)) {
        const char *known;
        if (!append_comp(lp, sizeof(lp), &llen, comp_l)) { out[0] = '\0'; return 0; }
        known = map_get(&g_fwd, lp);
        if (known) {
            size_t kl = strlen(known);
            if (kl >= sizeof(rp)) { out[0] = '\0'; return 0; }
            memcpy(rp, known, kl + 1);
            rlen = kl;
            continue;
        }
        if (!plain_from_local(comp_l, comp_r, sizeof(comp_r))
            || !append_comp(rp, sizeof(rp), &rlen, comp_r)) { out[0] = '\0'; return 0; }
        /* would the way back write a different spelling? then remember
         * the one on disk (PC: decomposed names) */
        if (!plain_to_local(comp_r, back, sizeof(back)) || strcmp(back, comp_l) != 0)
            map_put(&g_rev, rp, lp);
    }
    if (rlen >= outsz) { out[0] = '\0'; return 0; }
    memcpy(out, rp, rlen + 1);
    return 1;
}

int vcs_local_join(char *out, size_t outsz, const char *root, const char *repo_rel)
{
    static char local[NAMEBUF];
    if (vcs_path_to_local(repo_rel, local, sizeof(local)) < 0
        || strlen(root) + strlen(local) + 2 > outsz) {
        if (outsz) out[0] = '\0';
        return 0;
    }
    localfs_join(out, outsz, root, local);
    return 1;
}

/* ------------------------------------------------------------------ */
/* table file and collision check                                      */

void vcs_names_load(const char *amivcs_dir)
{
    char path[NAMEBUF];
    static char line[NAMEBUF * 2 + 4];
    FILE *fp;
    localfs_join(path, sizeof(path), amivcs_dir, LOCALNAMES);
    fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        char *tab;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        tab = strchr(line, '\t');
        if (!tab || tab == line || !tab[1]) continue;
        *tab = '\0';
        map_put(&g_fwd, line, tab + 1);
    }
    fclose(fp);
}

void vcs_names_forget(const char *amivcs_dir)
{
    char path[NAMEBUF];
    localfs_join(path, sizeof(path), amivcs_dir, LOCALNAMES);
    remove(path);
}

void vcs_names_save(const char *amivcs_dir)
{
    char path[NAMEBUF];
    FILE *fp = NULL;
    int i;
    localfs_join(path, sizeof(path), amivcs_dir, LOCALNAMES);
    for (i = 0; i < NB; i++) {
        const Node *n;
        for (n = g_fwd.b[i]; n; n = n->next) {
            if (strpbrk(n->key, "\t\n\r") || strpbrk(n->val, "\t\n\r")) continue;  /* not storable */
            if (!fp && !(fp = fopen(path, "w"))) {
                fprintf(stderr, "amivcs: cannot write %s\n", path);
                return;
            }
            fprintf(fp, "%s\t%s\n", n->key, n->val);
        }
    }
    if (fp) {
        if (fclose(fp) != 0) fprintf(stderr, "amivcs: cannot write %s\n", path);
    } else
        remove(path);
}

void vcs_names_prune(const char *amivcs_dir, int (*settled)(const char *repo_path, void *ctx), void *ctx)
{
    Map keep;
    int i;
    memset(&keep, 0, sizeof(keep));
    /* the file's pairs only, not the operation's table */
    {
        Map save = g_fwd;
        memset(&g_fwd, 0, sizeof(g_fwd));
        vcs_names_load(amivcs_dir);
        for (i = 0; i < NB; i++) {
            const Node *n;
            for (n = g_fwd.b[i]; n; n = n->next)
                if (!settled(n->val, ctx)) map_put(&keep, n->key, n->val);
        }
        map_clear(&g_fwd);
        g_fwd = keep;
        vcs_names_save(amivcs_dir);
        map_clear(&g_fwd);
        g_fwd = save;
    }
}

typedef struct { char *local; const char *repo; } Pair;

/* AmigaOS, macOS and Windows file systems ignore case: "README" and
 * "Readme" are one file there (ASCII letters; enough for the usual
 * clash) */
static void fold_case(char *s)
{
#if defined(PLATFORM_AMIGA) || defined(__APPLE__) || defined(_WIN32)
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
#else
    (void)s;
#endif
}

static int pair_cmp(const void *a, const void *b)
{
    int c = strcmp(((const Pair *)a)->local, ((const Pair *)b)->local);
    return c ? c : strcmp(((const Pair *)a)->repo, ((const Pair *)b)->repo);
}

int vcs_names_check(const char *const *paths, int n, char *a, char *b, size_t sz)
{
    static char local[NAMEBUF];
    Pair *pairs;
    char **repos;
    int np = 0, cap = 0, nr = 0, i, ok = 1;

    if (a && sz) a[0] = '\0';
    if (b && sz) b[0] = '\0';
    if (n <= 0) return 1;
    /* every file and every parent directory, repository form */
    cap = n * 2 + 16;
    pairs = (Pair *)malloc(sizeof(Pair) * (size_t)cap);
    repos = (char **)malloc(sizeof(char *) * (size_t)cap);
    if (!pairs || !repos) { free(pairs); free(repos); return 0; }   /* better refuse than guess */
    for (i = 0; i < n; i++) {
        const char *s = paths[i];
        const char *q;
        for (q = s; ; q++) {
            if (*q == '/' || *q == '\0') {
                size_t len = (size_t)(q - s);
                char *r;
                if (np == cap) {
                    Pair *np2;
                    char **nr2;
                    cap *= 2;
                    np2 = (Pair *)realloc(pairs, sizeof(Pair) * (size_t)cap);
                    if (np2) pairs = np2;
                    nr2 = (char **)realloc(repos, sizeof(char *) * (size_t)cap);
                    if (nr2) repos = nr2;
                    if (!np2 || !nr2) { ok = 0; goto done; }
                }
                r = (char *)malloc(len + 1);
                if (!r) { ok = 0; goto done; }
                memcpy(r, s, len);
                r[len] = '\0';
                repos[nr++] = r;
                if (vcs_path_to_local(r, local, sizeof(local)) < 0) local[0] = '\0';
                pairs[np].local = (char *)malloc(strlen(local) + 1);
                if (!pairs[np].local) { ok = 0; goto done; }
                strcpy(pairs[np].local, local);
                fold_case(pairs[np].local);
                pairs[np].repo = r;
                np++;
            }
            if (*q == '\0') break;
        }
    }
    qsort(pairs, (size_t)np, sizeof(Pair), pair_cmp);
    for (i = 1; i < np; i++) {
        if (strcmp(pairs[i].local, pairs[i - 1].local) == 0
            && strcmp(pairs[i].repo, pairs[i - 1].repo) != 0) {
            if (a && sz) vcs_utf8_copy(a, sz, pairs[i - 1].repo);
            if (b && sz) vcs_utf8_copy(b, sz, pairs[i].repo);
            ok = 0;
            break;
        }
    }
done:
    if (!ok && np && (!a || !a[0])) {      /* out of memory: say so */
        if (a && sz) vcs_utf8_copy(a, sz, "(out of memory)");
        if (b && sz) b[0] = '\0';
    }
    for (i = 0; i < np; i++) free(pairs[i].local);
    for (i = 0; i < nr; i++) free(repos[i]);
    free(pairs);
    free(repos);
    return ok;
}

/* ------------------------------------------------------------------ */
/* texts                                                               */

/* where to cut s (at most max bytes): before an ASCII byte if possible
 * (never between a letter and its combining mark), else at a character
 * boundary; '?' always ends a piece (codesets.library turns it into '_') */
static size_t piece_len(const char *s, size_t max, int stop_at_q)
{
    size_t n = strlen(s), i, last_ascii = 0;
    if (stop_at_q) {
        const char *q = strchr(s, '?');
        if (q && (size_t)(q - s) < n) n = (size_t)(q - s);
    }
    if (n <= max) return n;
    for (i = 1; i <= max; i++)
        if ((unsigned char)s[i] < 0x80) last_ascii = i;
    if (last_ascii) return last_ascii;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n ? n : max;
}

/* UTF-8 -> local, piecewise; out holds strlen(u) + 8 */
static void text_from_utf8(const char *u, char *out)
{
    static char piece[CHUNK * 3 + 1], conv[CHUNK * 3 + 8];
    size_t o = 0;
    while (*u) {
        size_t n;
        if (*u == '?') { out[o++] = '?'; u++; continue; }
        n = piece_len(u, CHUNK * 3, 1);
        memcpy(piece, u, n);
        piece[n] = '\0';
        cs_from_utf8(piece, conv, sizeof(conv));
        memcpy(out + o, conv, strlen(conv));
        o += strlen(conv);
        u += n;
    }
    out[o] = '\0';
}

/* local -> UTF-8, piecewise; out holds 3 * strlen(l) + 8 */
static void text_to_utf8(const char *l, char *out)
{
    static char piece[CHUNK + 1], conv[CHUNK * 3 + 8];
    size_t o = 0;
    while (*l) {
        size_t n = piece_len(l, CHUNK, 0);
        memcpy(piece, l, n);
        piece[n] = '\0';
        cs_to_utf8(piece, conv, sizeof(conv));
        memcpy(out + o, conv, strlen(conv));
        o += strlen(conv);
        l += n;
    }
    out[o] = '\0';
}

char *vcs_text_utf8(const char *s)
{
    size_t n;
    char *out;
    if (!s) s = "";
    n = strlen(s);
    out = (char *)malloc(3 * n + 8);
    if (!out) return NULL;
    if (vcs_utf8_valid(s)) memcpy(out, s, n + 1);
    else if (local_is_utf8()) latin1_to_utf8(s, out);    /* PC: older Amiga text */
    else text_to_utf8(s, out);                            /* Amiga: it is local text */
    return out;
}

char *vcs_text_to_local(const char *s)
{
    size_t n;
    char *out;
    if (!s) s = "";
    n = strlen(s);
    out = (char *)malloc(2 * n + 8);
    if (!out) return NULL;
    if (!vcs_utf8_valid(s)) {
        if (local_is_utf8()) latin1_to_utf8(s, out);
        else memcpy(out, s, n + 1);                        /* Amiga: already local */
    } else
        text_from_utf8(s, out);
    return out;
}

void vcs_text_to_local_buf(const char *s, char *out, size_t outsz)
{
    char *l;
    if (!outsz) return;
    l = vcs_text_to_local(s);
    if (local_is_utf8()) vcs_utf8_copy(out, outsz, l ? l : "");
    else {
        strncpy(out, l ? l : "", outsz - 1);
        out[outsz - 1] = '\0';
    }
    free(l);
}

char *vcs_text_from_local(const char *s)
{
    size_t n;
    char *out;
    if (!s) s = "";
    n = strlen(s);
    out = (char *)malloc(3 * n + 8);
    if (!out) return NULL;
    if (local_is_utf8() && !vcs_utf8_valid(s)) latin1_to_utf8(s, out);   /* a Latin-1 terminal */
    else text_to_utf8(s, out);
    return out;
}
