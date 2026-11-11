/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * amivcs -- thin CLI front-end for libcloudvcs (docs/adr/09_ADR_VCS_Design.md).
 * Pure argument parsing + dispatch, no VCS logic of its own (repo.h owns
 * all of that) -- so a future GUI front-end can link the same library and
 * reimplement only presentation, exactly like amicloud vs. amicloud-
 * commander-mui/-qt.
 *
 * v1 must be run from the repository root (repo.h's vcs_repo_open()
 * doesn't search upward through parent directories -- see repo.h for why).
 */
#include "amicloud.h"
#include "config.h"
#include "object.h"
#include "objstore.h"
#include "refs.h"
#include "repo.h"
#include "index.h"
#include "merge.h"
#include "mergestate.h"
#include "repodiff.h"
#include "pack.h"
#include "remote.h"
#include "vcsconfig.h"
#include "localfs.h"
#include "http.h"
#include "ui_hooks.h"
#include "charset.h"
#include "vcsname.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>

#ifdef PLATFORM_AMIGA
#include <dos/dos.h>
#include <proto/dos.h>
#elif defined(_WIN32)
#include <conio.h>
#include <io.h>
#else
#include <unistd.h>
#include <termios.h>
#endif

#ifdef PLATFORM_AMIGA
long __stack = 65536;
const char amiga_ver[] = "$VER: amivcs " AC_VERSION " (" AC_DATE ")";
#endif

/* Configuration (docs/design/remotes-plan.md, vcsconfig.h): the remote
 * URL, its user and -- if remembered -- password per repository in
 * .amivcs/config; the commit identity there or as a global default in
 * ENV:AmiVCS/config / ~/.amivcsconfig, which also holds charset=,
 * timeout=, verify_ssl=, debug=. */

/* ------------------------------------------------------------------ */
/* Machine-readable output for programs (the IDE): --porcelain-out FILE */
/* ------------------------------------------------------------------ */
/* One record per line, first field the record type, fields separated by
 * a tab; in every field '\', tab, newline and CR are escaped (\\ \t \n
 * \r). All text in the local codeset, like the rest of the output. The
 * file is written by amivcs itself and closed before exit: on the Amiga a
 * child's stdout and stderr cannot be told apart, and buffering of a
 * redirected stdout no longer matters. The last line is always
 * "end<TAB>rc". Format version: PORCELAIN_FORMAT (record "format"). */
#define PORCELAIN_FORMAT "1"

static FILE *g_pc;

static void pc_field(const char *s)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '\\': fputs("\\\\", g_pc); break;
        case '\t': fputs("\\t", g_pc); break;
        case '\n': fputs("\\n", g_pc); break;
        case '\r': fputs("\\r", g_pc); break;
        default:   fputc(*s, g_pc);
        }
    }
}

/* pc_rec("change", 3, "S", "M", path) */
static void pc_rec(const char *tag, int n, ...)
{
    va_list ap;
    int k;
    if (!g_pc) return;
    fputs(tag, g_pc);
    va_start(ap, n);
    for (k = 0; k < n; k++) {
        fputc('\t', g_pc);
        pc_field(va_arg(ap, const char *));
    }
    va_end(ap);
    fputc('\n', g_pc);
}

/* notify hook: every message as a "msg" record too, then as on the console */
static void cli_notify(int level, const char *msg)
{
    if (g_pc) pc_rec("msg", 2, level == UI_ERROR ? "error" : level == UI_WARN ? "warn" : "info", msg);
    if (level == UI_ERROR || level == UI_WARN) fprintf(stderr, "%s\n", msg);
    else printf("%s\n", msg);
}

static void pc_progress(const char *label, long done, long total, int final)
{
    char d[24], t[24];
    sprintf(d, "%ld", done);
    sprintf(t, "%ld", total);
    pc_rec("progress", 3, d, t, label);
    if (final) fflush(g_pc);
}

/* the console CLI prints its own progress lines (remote.c); the ui_hooks
 * progress callback only carries the cancel decision */
static int noop_progress(long done, long total, const char *name)
{
    (void)done; (void)total; (void)name;
    return !ui_break_pending(); /* Ctrl-C cancels a clone/pull/push download */
}

/* ------------------------------------------------------------------ */
/* Cancelling only where it is safe                                     */
/* ------------------------------------------------------------------ */
#ifdef PLATFORM_AMIGA
/* vbcc's stdio calls __chkabort() and ends the program on a pending
 * Ctrl-C at the next printf -- possibly halfway through writing the
 * working tree. Ctrl-C is checked only where stopping is safe
 * (ui_break_pending() in the download loops). */
void __chkabort(void) { }
#else
static void on_signal(int sig)
{
    (void)sig;
    ui_break_set();          /* the download loops stop at the next check */
    signal(sig, on_signal);
}
#endif

/* ------------------------------------------------------------------ */
/* Arguments from a file: --args-from FILE                              */
/* ------------------------------------------------------------------ */
/* One argument per line, escaped like porcelain fields. Appended after
 * the command-line arguments, so nothing on an AmigaDOS command line has
 * to be quoted (';' '$' '*' '<' '>' '"', and its length limit). */
static char **g_xargv;
static int g_xargc;

static int load_args(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[2048];
    if (!fp) { fprintf(stderr, "amivcs: cannot read %s\n", path); return 0; }
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        char *o, *p, **nv;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        for (o = p = line; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                *o++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p;
            } else *o++ = *p;
        }
        *o = '\0';
        nv = (char **)realloc(g_xargv, sizeof(char *) * (size_t)(g_xargc + 1));
        if (!nv) break;
        g_xargv = nv;
        g_xargv[g_xargc] = (char *)malloc(strlen(line) + 1);
        if (!g_xargv[g_xargc]) break;
        strcpy(g_xargv[g_xargc++], line);
    }
    fclose(fp);
    return 1;
}

/* whole file as text (commit -F): NULL on error */
static char *read_text(const char *path)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    long n;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)(n > 0 ? n : 0) + 1);
    if (buf) {
        n = (long)fread(buf, 1, (size_t)(n > 0 ? n : 0), fp);
        buf[n] = '\0';
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    }
    fclose(fp);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Porcelain records for status, branches, log                          */
/* ------------------------------------------------------------------ */
static void pc_head(VcsRepo *repo)
{
    VcsHead head;
    VcsRepoConfig rc;
    if (vcs_repo_config_load(repo->amivcs_dir, &rc) && rc.remote[0])
        pc_rec("remote", 1, rc.remote);
    if (!vcs_head_load(repo->amivcs_dir, &head)) return;
    if (head.detached) {
        char hex[VCS_OID_HEX];
        vcs_oid_to_hex(&head.oid, hex);
        pc_rec("detached", 1, hex);
    } else {
        pc_rec("branch", 1, vcs_shown(head.branch));
    }
}

static void pc_changes(const VcsChangeList *l, const char *which)
{
    int k;
    for (k = 0; k < l->n; k++) {
        char kind[2], local[1024];
        kind[0] = l->items[k].kind;
        kind[1] = '\0';
        /* the name table of the status run is still valid here */
        if (vcs_path_to_local(l->items[k].path, local, sizeof(local)) < 0) continue;
        pc_rec("change", 3, which, kind, local);
    }
}

/* "merging<TAB>label<TAB>theirs-oid<TAB>open-count", then per conflict
 * "conflict<TAB>U|R<TAB>kind<TAB>path<TAB>theirs-file(0|1)" */
static void pc_merge(const VcsMergeState *m)
{
    char theirs[VCS_OID_HEX], open[16];
    int k;
    if (!m->active) return;
    vcs_oid_to_hex(&m->theirs, theirs);
    sprintf(open, "%d", vcs_mergestate_unresolved(m));
    pc_rec("merging", 3, vcs_shown(m->label), theirs, open);
    for (k = 0; k < m->n; k++) {
        const VcsMergeFile *f = &m->items[k];
        char st[2], local[1024];
        if (!vcs_mergestate_is_conflict(f)) continue;
        if (vcs_path_to_local(f->path, local, sizeof(local)) < 0) continue;
        st[0] = f->status;
        st[1] = '\0';
        pc_rec("conflict", 4, st, f->kind, local, f->theirs_file ? "1" : "0");
    }
}

static int pc_status(VcsRepo *repo)
{
    VcsStatus st;
    if (!vcs_repo_status_raw(repo, &st)) return 0;
    pc_head(repo);
    pc_changes(&st.staged, "S");
    pc_changes(&st.unstaged, "U");
    pc_merge(&st.merge);
    vcs_status_free(&st);
    return 1;
}

static void pc_branches(VcsRepo *repo)
{
    VcsRefList refs;
    VcsHead head;
    int k;
    if (!vcs_refs_load(repo->amivcs_dir, &refs)) return;
    if (!vcs_head_load(repo->amivcs_dir, &head)) head.detached = 1;
    for (k = 0; k < refs.n; k++) {
        const char *full = refs.items[k].name;
        const char *name = strncmp(full, VCS_REF_PREFIX, strlen(VCS_REF_PREFIX)) == 0
                           ? full + strlen(VCS_REF_PREFIX) : full;
        pc_rec("ref", 2, (!head.detached && strcmp(head.branch, name) == 0) ? "*" : "-",
               vcs_shown(name));
    }
    vcs_refs_free(&refs);
}

static int pc_log(VcsRepo *repo, int all, long limit)
{
    VcsLogList log;
    int k;
    if (!(all ? vcs_repo_log_all_raw(repo, &log) : vcs_repo_log_raw(repo, &log))) return 0;
    for (k = 0; k < log.n && (limit <= 0 || k < limit); k++) {
        const VcsLogEntry *e = &log.items[k];
        char hex[VCS_OID_HEX], parents[VCS_MAX_PARENTS * VCS_OID_HEX + 8], mt[24];
        char refs[256], *msg;
        int j;
        const char *d;
        char *o;
        vcs_oid_to_hex(&e->oid, hex);
        parents[0] = '\0';
        for (j = 0; j < e->nparents; j++) {
            char ph[VCS_OID_HEX];
            vcs_oid_to_hex(&e->parents[j], ph);
            if (j) strcat(parents, ",");
            strcat(parents, ph);
        }
        sprintf(mt, "%ld", e->mtime);
        /* ", "-joined decoration -> "," */
        for (d = e->decoration, o = refs; *d && o < refs + sizeof(refs) - 1; d++) {
            if (d[0] == ',' && d[1] == ' ') { *o++ = ','; d++; }
            else *o++ = *d;
        }
        *o = '\0';
        msg = vcs_text_to_local(e->message ? e->message : "");
        pc_rec("commit", 6, hex, parents, mt, vcs_shown(e->author), vcs_shown(refs), msg ? msg : "");
        free(msg);
    }
    vcs_loglist_free(&log);
    return 1;
}

/* ------------------------------------------------------------------ */
/* diff / show output                                                    */
/* ------------------------------------------------------------------ */
static void out_file(const VcsFileDiff *f, void *ctx)
{
    char local[1024];
    (void)ctx;
    if (vcs_path_to_local(f->path, local, sizeof(local)) < 0) strcpy(local, "?");
    if (g_pc) {
        char kind[2], o[VCS_OID_HEX], n[VCS_OID_HEX];
        kind[0] = f->kind;
        kind[1] = '\0';
        vcs_oid_to_hex(&f->old_oid, o);
        vcs_oid_to_hex(&f->new_oid, n);
        pc_rec("file", 5, kind, local, o, n, f->binary ? "1" : "0");
        return;
    }
    if (f->binary) {
        printf("Binary files %s%s and %s%s differ\n",
               f->kind == 'N' ? "" : "a/", f->kind == 'N' ? "/dev/null" : local,
               f->kind == 'D' ? "" : "b/", f->kind == 'D' ? "/dev/null" : local);
        return;
    }
    printf("--- %s%s\n", f->kind == 'N' ? "" : "a/", f->kind == 'N' ? "/dev/null" : local);
    printf("+++ %s%s\n", f->kind == 'D' ? "" : "b/", f->kind == 'D' ? "/dev/null" : local);
}

static void out_hunk(long as, long ac, long bs, long bc, void *ctx)
{
    (void)ctx;
    if (g_pc) {
        char a[24], b[24], c[24], d[24];
        sprintf(a, "%ld", as); sprintf(b, "%ld", ac); sprintf(c, "%ld", bs); sprintf(d, "%ld", bc);
        pc_rec("hunk", 4, a, b, c, d);
        return;
    }
    printf("@@ -%ld,%ld +%ld,%ld @@\n", as, ac, bs, bc);
}

static void out_line(char op, const char *text, long len, int eol, void *ctx)
{
    (void)ctx;
    if (g_pc) {
        char o[2], *t = (char *)malloc((size_t)len + 1);
        o[0] = op;
        o[1] = '\0';
        if (!t) return;
        memcpy(t, text, (size_t)len);
        t[len] = '\0';
        pc_rec("line", 3, o, t, eol ? "1" : "0");
        free(t);
        return;
    }
    putchar(op);
    fwrite(text, 1, (size_t)len, stdout);
    putchar('\n');
    if (!eol) printf("\\ No newline at end of file\n");
}

static const VcsRepoDiffSink g_out_sink = { out_file, { out_hunk, out_line } };

/* local path arguments -> repository paths (malloc'd array) */
static char **repo_paths(VcsRepo *repo, char **args, int n)
{
    char **rp = (char **)calloc((size_t)(n + 1), sizeof(char *));
    int k;
    if (!rp) return NULL;
    vcs_repo_learn_names(repo);
    for (k = 0; k < n; k++) {
        rp[k] = (char *)malloc(1024);
        if (!rp[k]) break;
        size_t n;
        {
            const char *a = args[k];
            while (a[0] == '.' && a[1] == '/') a += 2;   /* "./x" is "x" */
            if (!vcs_path_from_local(a, rp[k], 1024)) strcpy(rp[k], a);
        }
        /* "dir/" and "." mean what is below */
        if (strcmp(rp[k], ".") == 0) rp[k][0] = '\0';
        n = strlen(rp[k]);
        while (n > 0 && rp[k][n - 1] == '/') rp[k][--n] = '\0';
    }
    return rp;
}

static void free_paths(char **rp, int n)
{
    int k;
    if (!rp) return;
    for (k = 0; k < n; k++) free(rp[k]);
    free(rp);
}

/* diff [--staged|--cached] [<rev> [<rev>]] [--] [<path>...] */
static int cmd_diff(VcsRepo *repo, int argc, char **argv, int i)
{
    int staged = 0, nrev = 0, npaths = 0, dashdash = 0, has_dashdash = 0, rc = 0;
    const char *revs[2];
    char **paths_local = (char **)malloc(sizeof(char *) * (size_t)(argc + 1));
    char **rp;
    VcsDiffSide from, to;

    if (!paths_local) return 1;
    {   /* with "--", everything before it is a revision */
        int k;
        for (k = i; k < argc; k++) if (strcmp(argv[k], "--") == 0) has_dashdash = 1;
    }
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!dashdash && (strcmp(a, "--staged") == 0 || strcmp(a, "--cached") == 0)) { staged = 1; continue; }
        if (!dashdash && strcmp(a, "--") == 0) { dashdash = 1; continue; }
        /* without "--": revisions first, as long as the argument is no
         * file here and names a commit; then only paths */
        if (!dashdash && nrev < 2 && npaths == 0
            && (has_dashdash || (!localfs_exists(a) && vcs_repo_rev_exists(repo, a)))) {
            revs[nrev++] = a;
            continue;
        }
        paths_local[npaths++] = argv[i];
    }
    if (staged && nrev > 1) { ui_notify(UI_ERROR, "amivcs: diff --staged takes at most one revision"); free(paths_local); return 1; }

    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));
    if (nrev == 0 && !staged) {
        from.kind = VCS_SIDE_INDEX;
        to.kind = VCS_SIDE_WORKDIR;
    } else {
        from.kind = VCS_SIDE_COMMIT;
        if (nrev > 0) {
            if (!vcs_repo_resolve_rev(repo, revs[0], &from.oid)) { free(paths_local); return 1; }
        } else if (!vcs_head_resolve(repo->amivcs_dir, &from.oid)) {
            from.oid = VCS_OID_ZERO;
        }
        if (nrev == 2) {
            to.kind = VCS_SIDE_COMMIT;
            if (!vcs_repo_resolve_rev(repo, revs[1], &to.oid)) { free(paths_local); return 1; }
        } else {
            to.kind = staged ? VCS_SIDE_INDEX : VCS_SIDE_WORKDIR;
        }
    }

    rp = repo_paths(repo, paths_local, npaths);
    if (vcs_repo_diff(repo, &from, &to, (const char *const *)rp, npaths, &g_out_sink, NULL) < 0) {
        ui_notify(UI_ERROR, "amivcs: diff failed");
        rc = 1;
    }
    free_paths(rp, npaths);
    free(paths_local);
    return rc;
}

/* show [<rev>]: the commit, then its changes against the first parent */
static int cmd_show(VcsRepo *repo, int argc, char **argv, int i)
{
    VcsOid oid;
    VcsObjType type;
    void *data;
    long len;
    VcsCommit c;
    VcsDiffSide from, to;
    char hex[VCS_OID_HEX];
    int rc = 0;

    if (!vcs_repo_resolve_rev(repo, i < argc ? argv[i] : "HEAD", &oid)) return 1;
    if (!vcsobjstore_read(&repo->store, &oid, &type, &data, &len)) { ui_notify(UI_ERROR, "amivcs: no such commit"); return 1; }
    vcs_commit_init(&c);
    if (type != VCS_OBJ_COMMIT || !vcs_commit_decode(data, len, &c)) {
        free(data);
        ui_notify(UI_ERROR, "amivcs: not a commit");
        return 1;
    }
    free(data);
    vcs_oid_to_hex(&oid, hex);
    {
        char *author = vcs_text_to_local(c.author);
        char *msg = vcs_text_to_local(c.message ? c.message : "");
        char parents[VCS_MAX_PARENTS * VCS_OID_HEX + 8];
        int j;
        parents[0] = '\0';
        for (j = 0; j < c.nparents; j++) {
            char ph[VCS_OID_HEX];
            vcs_oid_to_hex(&c.parents[j], ph);
            if (j) strcat(parents, g_pc ? "," : " ");
            strcat(parents, ph);
        }
        if (g_pc) {
            char mt[24];
            sprintf(mt, "%ld", c.mtime);
            pc_rec("commit", 6, hex, parents, mt, author ? author : c.author, "", msg ? msg : "");
        } else {
            time_t t = (time_t)c.mtime;
            char timebuf[32];
            const char *p;
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", gmtime(&t));
            printf("commit %s\n", hex);
            if (c.nparents > 1) printf("Merge:  %s\n", parents);
            printf("Author: %s\n", author ? author : c.author);
            printf("Date:   %s UTC\n\n", timebuf);
            /* message indented line by line */
            p = msg ? msg : "";
            while (*p) {
                const char *e = strchr(p, '\n');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                printf("    %.*s\n", (int)n, p);
                p += n + (e ? 1 : 0);
            }
            printf("\n");
        }
        free(author);
        free(msg);
    }
    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));
    from.kind = to.kind = VCS_SIDE_COMMIT;
    from.oid = c.nparents > 0 ? c.parents[0] : VCS_OID_ZERO;
    to.oid = oid;
    vcs_commit_free(&c);
    if (vcs_repo_diff(repo, &from, &to, NULL, 0, &g_out_sink, NULL) < 0) {
        ui_notify(UI_ERROR, "amivcs: diff failed");
        rc = 1;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Credentials for clone/push/pull                                       */
/* ------------------------------------------------------------------ */
/* Given by a program (the IDE, via --args-from) or typed in; never put
 * on a real command line. */
static const char *g_opt_user, *g_opt_password;
static int g_opt_remember = -1;   /* -1: ask, 0/1: given */

static int interactive(void)
{
    if (g_pc) return 0;           /* a program is driving amivcs */
#ifdef PLATFORM_AMIGA
    return IsInteractive(Input()) != 0;
#elif defined(_WIN32)
    return _isatty(0);
#else
    return isatty(0);
#endif
}

/* One line from the console; echo off for passwords. */
static int read_line(const char *prompt, char *out, size_t sz, int secret)
{
    size_t n = 0;
    fputs(prompt, stdout);
    fflush(stdout);
#ifdef PLATFORM_AMIGA
    if (secret) {
        BPTR in = Input();
        char c;
        SetMode(in, 1);           /* raw: no echo, character by character */
        while (Read(in, &c, 1) == 1) {
            if (c == '\r' || c == '\n') break;
            if (c == 3) { n = 0; break; }                 /* Ctrl-C */
            if (c == 8 || c == 127) { if (n) n--; continue; }
            if (n + 1 < sz && (unsigned char)c >= 32) out[n++] = c;
        }
        SetMode(in, 0);
        out[n] = '\0';
        fputs("\n", stdout);
        return 1;
    }
#elif defined(_WIN32)
    if (secret && _isatty(0)) {
        int c;                    /* _getch() reads without echo */
        while ((c = _getch()) != EOF) {
            if (c == '\r' || c == '\n') break;
            if (c == 3) { n = 0; break; }                 /* Ctrl-C */
            if (c == 0 || c == 0xE0) { _getch(); continue; } /* arrow keys etc. */
            if (c == 8 || c == 127) { if (n) n--; continue; }
            if (n + 1 < sz && c >= 32) out[n++] = (char)c;
        }
        out[n] = '\0';
        fputs("\n", stdout);
        return 1;
    }
#else
    if (secret) {
        struct termios old, quiet;
        int ok = tcgetattr(0, &old) == 0;
        if (ok) { quiet = old; quiet.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(0, TCSANOW, &quiet); }
        if (!fgets(out, (int)sz, stdin)) out[0] = '\0';
        if (ok) tcsetattr(0, TCSANOW, &old);
        fputs("\n", stdout);
        n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
        return 1;
    }
#endif
    if (!fgets(out, (int)sz, stdin)) { out[0] = '\0'; return 0; }
    n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return 1;
}

/* Makes sure cfg has a user and a password for `url`: options first, then
 * what .amivcs/config has (already in cfg), then -- at a console -- the
 * user. force: ask again (the server said 401). *remember: 1 if the
 * password should go into .amivcs/config. Without a console a program
 * gets an "auth-required" record and handles the dialog itself. */
static int get_credentials(AcConfig *cfg, const char *url, int force, int *remember)
{
    char ans[16];

    if (g_opt_user && !force) strncpy(cfg->username, g_opt_user, sizeof(cfg->username) - 1);
    if (g_opt_password && !force) strncpy(cfg->password, g_opt_password, sizeof(cfg->password) - 1);
    *remember = g_opt_remember > 0;
    if (cfg->username[0] && cfg->password[0] && !force) return 1;

    if (!interactive()) {
        pc_rec("auth-required", 2, url, cfg->username);
        ui_notify(UI_ERROR, force ? "amivcs: login refused for %s -- wrong user or password"
                                  : "amivcs: %s needs a user and password", url);
        return 0;
    }
    if (force) ui_notify(UI_WARN, "amivcs: login refused -- wrong user or password?");
    printf("Login for %s\n", url);
    if (!cfg->username[0] || force) {
        char prompt[96], user[64];
        if (cfg->username[0]) snprintf(prompt, sizeof(prompt), "User [%s]: ", cfg->username);
        else strcpy(prompt, "User: ");
        read_line(prompt, user, sizeof(user), 0);
        if (user[0]) strncpy(cfg->username, user, sizeof(cfg->username) - 1);
    }
    read_line("Password: ", cfg->password, sizeof(cfg->password), 1);
    if (!cfg->username[0] || !cfg->password[0]) return 0;
    if (g_opt_remember < 0) {
        read_line("Remember the password in .amivcs/config (plain text)? [y/N] ", ans, sizeof(ans), 0);
        *remember = ans[0] == 'y' || ans[0] == 'Y' || ans[0] == 'j' || ans[0] == 'J';
    }
    return 1;
}

/* After a successful clone/push/pull: user (and, if wanted, password)
 * into the repository's config. */
static void store_credentials(const char *amivcs_dir, const AcConfig *cfg, int remember)
{
    if (cfg->username[0]) vcs_repo_config_set(amivcs_dir, "user", cfg->username);
    if (remember) vcs_repo_config_set(amivcs_dir, "pass", cfg->password);
}

/* Takes --user/--password/--remember-password/--no-remember-password out
 * of argv (they may follow the command, e.g. from --args-from). */
static int strip_credential_options(int argc, char **argv)
{
    int r, w = 0;
    for (r = 0; r < argc; r++) {
        if (strcmp(argv[r], "--user") == 0 && r + 1 < argc) { g_opt_user = argv[++r]; continue; }
        if (strcmp(argv[r], "--password") == 0 && r + 1 < argc) { g_opt_password = argv[++r]; continue; }
        if (strcmp(argv[r], "--remember-password") == 0) { g_opt_remember = 1; continue; }
        if (strcmp(argv[r], "--no-remember-password") == 0) { g_opt_remember = 0; continue; }
        argv[w++] = argv[r];
    }
    argv[w] = NULL;
    return w;
}

static int is_url(const char *s) { return s && strstr(s, "://") != NULL; }

/* ------------------------------------------------------------------ */
/* config / remote                                                       */
/* ------------------------------------------------------------------ */
static const char *const repo_keys[] = { "remote", "user", "pass", "name", "email", NULL };
static const char *const global_keys[] = { "name", "email", "charset", "timeout", "verify_ssl", "debug", NULL };

static int key_allowed(const char *key, int global)
{
    const char *const *k;
    for (k = global ? global_keys : repo_keys; *k; k++) if (strcmp(*k, key) == 0) return 1;
    return 0;
}

static void print_config_file(const char *path, int global)
{
    const char *const *k;
    for (k = global ? global_keys : repo_keys; *k; k++) {
        char v[AC_MAX_URL];
        if (vcs_config_file_get(path, *k, v, sizeof(v)))
            printf("%s=%s\n", *k, strcmp(*k, "pass") == 0 ? "********" : v);
    }
}

/* amivcs config [--global] [--unset] [<key> [<value>]] */
static int cmd_config(int argc, char **argv, int i, const char *amivcs_dir)
{
    int global = 0, unset = 0;
    const char *key = NULL, *val = NULL;
    char path[AC_MAX_PATH];

    for (; i < argc; i++) {
        if (strcmp(argv[i], "--global") == 0) global = 1;
        else if (strcmp(argv[i], "--unset") == 0) unset = 1;
        else if (!key) key = argv[i];
        else if (!val) val = argv[i];
    }
    if (!global && !amivcs_dir) {
        ui_notify(UI_ERROR, "amivcs: not in a repository -- use --global for the defaults");
        return 1;
    }
    if (global) vcs_global_config_path(path, sizeof(path));
    else vcs_repo_config_path(amivcs_dir, path, sizeof(path));

    if (!key) { print_config_file(path, global); return 0; }
    if (!key_allowed(key, global)) {
        ui_notify(UI_ERROR, "amivcs: unknown %s key '%s'", global ? "global" : "repository", key);
        return 1;
    }
    if (!val && !unset) {
        char v[AC_MAX_URL];
        if (!vcs_config_file_get(path, key, v, sizeof(v))) return 1;
        printf("%s\n", strcmp(key, "pass") == 0 ? "********" : v);
        return 0;
    }
    if (val && strcmp(key, "remote") == 0) {
        char server[AC_MAX_URL], name[256];
        if (!vcs_url_split(val, server, sizeof(server), name, sizeof(name))) {
            ui_notify(UI_ERROR, "amivcs: '%s' is not a repository URL (https://server/<name>)", val);
            return 1;
        }
    }
    if (global ? !vcs_global_set(key, unset ? NULL : val)
               : !vcs_config_file_set(path, key, unset ? NULL : val)) {
        ui_notify(UI_ERROR, "amivcs: cannot write %s", path);
        return 1;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "amivcs v" AC_VERSION " -- minimal, Amiga-optimised version control\n"
        "Usage: %s [options] <command> [args]\n"
        "\n"
        "Options:\n"
        "  --author <name>      Commit author (default: name/email from the config)\n"
        "  --porcelain-out <f>  Write machine-readable records to <f> (for programs)\n"
        "  --args-from <f>      Further arguments from <f>, one per line\n"
        "\n"
        "Commands:\n"
        "  init                 Create a new repository in the current directory\n"
        "  status               Show staged/unstaged changes since the last commit\n"
        "  add <path>...        Stage file(s)/directory(ies); a deleted path stages its deletion\n"
        "  add -A               Stage all changes (new/modified/deleted)\n"
        "  unstage <path>...    Take staged changes back out (working tree is left alone)\n"
        "  commit -m <msg> [-a] Commit what's staged; -a stages everything first\n"
        "  commit -F <file>     Same, message read from <file>\n"
        "  log [--all] [-n N]   Show commit history\n"
        "  branch [<name>]      List branches, or create a new one at HEAD\n"
        "  switch <branch>      Change the current branch and update the working tree\n"
        "  checkout             Restore the tracked files of HEAD\n"
        "  merge <branch>       Merge <branch> into the current branch\n"
        "  merge --abort        Give up a merge with conflicts, back to before it\n"
        "  resolve --ours|--theirs <path>...  Settle a conflict with one side's file\n"
        "  diff [<path>...]     Changes not staged yet (working tree vs staged)\n"
        "  diff --staged [<rev>] [<path>...]  Staged changes (vs HEAD or <rev>)\n"
        "  diff <rev> [<rev>] [-- <path>...]  Changes between commits / to the working tree\n"
        "  show [<rev>]         A commit and its changes (<rev>: HEAD, branch, id, id^, id~2)\n"
        "  gc                   Bundle loose objects into a pack\n"
        "  cat-file <oid>       Print a stored object's type and size (debug)\n"
        "  clone <url> [dir]    Clone https://server/<name> into [dir] (default <name>)\n"
        "  push [<url>]         Push the current branch; a URL becomes the remote\n"
        "  pull [<url>] [<branch>]  Fetch and merge <branch> (default: current)\n"
        "  remote [<url>]       Show or set this repository's remote URL\n"
        "  config [--global] [--unset] [<key> [<value>]]\n"
        "                       Show or set name, email (global or here),\n"
        "                       remote, user, pass (here); --unset pass forgets it\n"
        "  version              Print the version\n"
        "\n"
        "Must be run from the repository root (no parent-directory search).\n"
        "clone/push/pull ask for user and password when needed; programs pass\n"
        "--user/--password/--remember-password through --args-from.\n"
        "Defaults: ENV:AmiVCS/config (Amiga), ~/.amivcsconfig (elsewhere).\n",
        prog);
    exit(1);
}

/* commands that change the repository or the working tree take the lock */
static int is_writing(const char *cmd)
{
    static const char *const w[] = { "add", "unstage", "remove", "commit", "switch", "checkout",
                                     "merge", "pull", "gc", "resolve", NULL };
    int k;
    for (k = 0; w[k]; k++) if (strcmp(cmd, w[k]) == 0) return 1;
    return 0;
}

static int run(int argc, char **argv, AcConfig *cfg);
static const char *g_opt_author;

int main(int argc, char **argv)
{
    AcConfig cfg;
    const char *opt_pc = NULL, *opt_args = NULL;
    int i, rc, k, nargc;
    char **nargv;

    {
        UiHooks hooks;
        memset(&hooks, 0, sizeof(hooks));
        hooks.progress = noop_progress;
        hooks.notify = cli_notify;
        ui_set_hooks(&hooks);
    }
#ifndef PLATFORM_AMIGA
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif

    memset(&cfg, 0, sizeof(cfg));
    cfg.basepath[0] = '/';
    cfg.basepath[1] = '\0';
    cfg.verify_ssl  = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) { g_opt_author = argv[++i]; continue; }
        if (strcmp(argv[i], "--porcelain-out") == 0 && i + 1 < argc) { opt_pc = argv[++i]; continue; }
        if (strcmp(argv[i], "--args-from") == 0 && i + 1 < argc) { opt_args = argv[++i]; continue; }
        if (argv[i][0] == '-' && strcmp(argv[i], "--help") != 0) continue; /* unknown option: ignore */
        break;
    }

    if (opt_pc) {
        g_pc = fopen(opt_pc, "w");
        if (!g_pc) { fprintf(stderr, "amivcs: cannot write %s\n", opt_pc); return 1; }
        pc_rec("format", 1, PORCELAIN_FORMAT);
        vcs_progress_hook = pc_progress;
    }
    if (opt_args && !load_args(opt_args)) {
        if (g_pc) { pc_rec("end", 1, "1"); fclose(g_pc); }
        return 1;
    }

    vcs_global_load_options(&cfg);

    /* names and texts in the repository are UTF-8; the local side is the
     * file system's codeset (codesets.library, else ISO-8859-1; vcsname.h) */
    cs_open(cfg.charset);

    /* the command and its arguments: rest of argv plus --args-from */
    nargc = argc - i + g_xargc;
    nargv = (char **)malloc(sizeof(char *) * (size_t)(nargc + 1));
    if (!nargv) return 1;
    for (k = 0; k < argc - i; k++) nargv[k] = argv[i + k];
    for (k = 0; k < g_xargc; k++) nargv[argc - i + k] = g_xargv[k];
    nargv[nargc] = NULL;
    nargc = strip_credential_options(nargc, nargv);

    if (nargc < 1) { fprintf(stderr, "amivcs: no command given\n"); usage(argv[0]); }
    rc = run(nargc, nargv, &cfg);

    if (g_pc) {
        char r[8];
        sprintf(r, "%d", rc);
        pc_rec("end", 1, r);
        fclose(g_pc);
    }
    cs_close();
    return rc;
}

/* argv[0] is the command */
static int run(int argc, char **argv, AcConfig *cfg)
{
    const char *cmd = argv[0];
    VcsRepo repo;
    int i = 1, rc = 0, locked = 0;

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--help") == 0) {
        printf("AMIVCS v" AC_VERSION "\n");
        return 0;
    }

    if (strcmp(cmd, "init") == 0) {
        if (!vcs_repo_init(".")) {
            ui_notify(UI_ERROR, "amivcs: init failed (already a repository here?)");
            return 1;
        }
        ui_notify(UI_INFO, "Initialized empty AMIVCS repository in .amivcs/");
        return 0;
    }

    if (strcmp(cmd, "clone") == 0) {
        char repo_name[256], local_dir[AC_MAX_PATH];
        const char *url;
        int tries, remember = 0;
        if (i >= argc || !is_url(argv[i])) {
            ui_notify(UI_ERROR, "amivcs: clone requires <url> [dir], e.g. https://server/<name>");
            return 1;
        }
        url = argv[i++];
        if (!vcs_url_split(url, cfg->server, sizeof(cfg->server), repo_name, sizeof(repo_name))) {
            ui_notify(UI_ERROR, "amivcs: '%s' is not a repository URL (https://server/<name>)", url);
            return 1;
        }
        /* like git: the directory is named after the repository */
        if (i < argc) { strncpy(local_dir, argv[i], sizeof(local_dir) - 1); local_dir[sizeof(local_dir) - 1] = '\0'; }
        else vcs_text_to_local_buf(repo_name, local_dir, sizeof(local_dir));
        if (!http_init()) { ui_notify(UI_ERROR, "amivcs: network initialisation failed"); return 1; }
        rc = 1;
        for (tries = 0; tries < 3; tries++) {
            if (!get_credentials(cfg, url, tries > 0, &remember)) break;
            if (vcs_remote_clone(cfg, repo_name, local_dir)) {
                char amivcs_dir[AC_MAX_PATH];
                localfs_join(amivcs_dir, sizeof(amivcs_dir), local_dir, ".amivcs");
                store_credentials(amivcs_dir, cfg, remember);
                rc = 0;
                break;
            }
            if (!vcs_remote_auth_failed()) break;
        }
        http_cleanup();
        return rc;
    }

    if (strcmp(cmd, "config") == 0 && i < argc && strcmp(argv[i], "--global") == 0) {
        return cmd_config(argc, argv, i, NULL);
    }

    if (!vcs_repo_open(&repo, ".")) {
        ui_notify(UI_ERROR, "amivcs: not an AMIVCS repository (no .amivcs/ here -- run from the repo root)");
        return 1;
    }
    if (is_writing(cmd) && !(strcmp(cmd, "branch") == 0)) {
        if (!vcs_repo_lock(repo.amivcs_dir)) return 1;
        locked = 1;
    }

    if (strcmp(cmd, "status") == 0) {
        rc = g_pc ? !pc_status(&repo) : !vcs_repo_status(&repo);

    } else if (strcmp(cmd, "commit") == 0) {
        const char *message = NULL, *msgfile = NULL;
        char *filemsg = NULL;
        int stage_all = 0;
        int j = i;
        while (j < argc) {
            if (strcmp(argv[j], "-m") == 0 && j + 1 < argc) { message = argv[j + 1]; j += 2; }
            else if (strcmp(argv[j], "-F") == 0 && j + 1 < argc) { msgfile = argv[j + 1]; j += 2; }
            else if (strcmp(argv[j], "-a") == 0 || strcmp(argv[j], "--all") == 0) { stage_all = 1; j++; }
            else j++;
        }
        if (msgfile) {
            filemsg = read_text(msgfile);
            if (!filemsg) { ui_notify(UI_ERROR, "amivcs: cannot read %s", msgfile); rc = 1; }
            message = filemsg;
        }
        if (!rc && (!message || !message[0])) {
            if (vcs_mergestate_active(repo.amivcs_dir)) message = "";   /* "Merge branch '...'" */
            else { ui_notify(UI_ERROR, "amivcs: commit requires -m <message> or -F <file>"); rc = 1; }
        }
        if (!rc) {
            VcsOid oid;
            char ident[300];
            char *author = NULL;
            if (g_opt_author) author = vcs_text_from_local(g_opt_author);
            else if (vcs_identity(repo.amivcs_dir, ident, sizeof(ident))) author = vcs_text_from_local(ident);
            else ui_notify(UI_ERROR, "amivcs: who are you? Set your name first:\n"
                                     "  amivcs config --global name \"Your Name\"\n"
                                     "  amivcs config --global email you@example.org\n"
                                     "(without --global for this repository only)");
            char *msg = vcs_text_from_local(message);
            if (!author || !msg || !vcs_repo_commit(&repo, author, msg, stage_all, &oid)) rc = 1;
            else {
                char hex[VCS_OID_HEX];
                vcs_oid_to_hex(&oid, hex);
                printf("committed %s\n", hex);
                pc_rec("committed", 1, hex);
            }
            free(author);
            free(msg);
        }
        free(filemsg);

    } else if (strcmp(cmd, "add") == 0) {
        if (i >= argc) { ui_notify(UI_ERROR, "amivcs: add requires <path>... or -A"); rc = 1; }
        else if (strcmp(argv[i], "-A") == 0) {
            rc = !vcs_index_add_all(&repo);
        } else {
            vcs_repo_learn_names(&repo);
            for (; i < argc; i++) {
                char rp[1024];
                vcs_path_from_local(argv[i], rp, sizeof(rp));
                if (!vcs_index_add(&repo, rp)) {
                    ui_notify(UI_ERROR, "amivcs: could not add '%s'", argv[i]);
                    rc = 1;
                }
            }
        }

    } else if (strcmp(cmd, "unstage") == 0 || strcmp(cmd, "remove") == 0) {
        /* "remove" is the old name; both take staged changes back to
         * HEAD's version (before 0.6 "remove" staged a deletion) */
        if (i >= argc) { ui_notify(UI_ERROR, "amivcs: unstage requires <path>..."); rc = 1; }
        else {
            vcs_repo_learn_names(&repo);
            for (; i < argc; i++) {
                char rp[1024];
                vcs_path_from_local(argv[i], rp, sizeof(rp));
                if (!vcs_index_unstage(&repo, rp)) {
                    ui_notify(UI_ERROR, "amivcs: could not unstage '%s'", argv[i]);
                    rc = 1;
                }
            }
        }

    } else if (strcmp(cmd, "log") == 0) {
        int all = 0;
        long limit = 0;
        for (; i < argc; i++) {
            if (strcmp(argv[i], "--all") == 0) all = 1;
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) limit = atol(argv[++i]);
        }
        rc = g_pc ? !pc_log(&repo, all, limit) : !vcs_repo_log(&repo);

    } else if (strcmp(cmd, "branch") == 0) {
        if (i < argc) {
            char *name = vcs_repo_branch_name(&repo, argv[i]);   /* UTF-8 (or an older spelling) */
            if (!vcs_repo_lock(repo.amivcs_dir)) rc = 1;
            else {
                locked = 1;
                rc = !name || !vcs_repo_branch(&repo, name);
            }
            free(name);
        } else if (g_pc) {
            pc_branches(&repo);
        } else {
            rc = !vcs_repo_branch(&repo, NULL);
        }

    } else if (strcmp(cmd, "switch") == 0) {
        if (i >= argc) { ui_notify(UI_ERROR, "amivcs: switch requires <branch>"); rc = 1; }
        else {
            char *b = vcs_repo_branch_name(&repo, argv[i]);
            rc = !b || !vcs_repo_switch(&repo, b);
            free(b);
        }

    } else if (strcmp(cmd, "checkout") == 0) {
        rc = !vcs_repo_checkout(&repo);

    } else if (strcmp(cmd, "merge") == 0) {
        if (i >= argc) { ui_notify(UI_ERROR, "amivcs: merge requires <branch> (or --abort)"); rc = 1; }
        else if (strcmp(argv[i], "--abort") == 0) {
            rc = !vcs_repo_merge_abort(&repo);
        } else {
            char *b = vcs_repo_branch_name(&repo, argv[i]);
            rc = !b || !vcs_repo_merge(&repo, b);
            free(b);
        }

    } else if (strcmp(cmd, "resolve") == 0) {
        int side = -1;
        if (i < argc && strcmp(argv[i], "--ours") == 0) side = 0;
        else if (i < argc && strcmp(argv[i], "--theirs") == 0) side = 1;
        if (side < 0 || i + 1 >= argc) {
            ui_notify(UI_ERROR, "amivcs: resolve requires --ours or --theirs and <path>...");
            rc = 1;
        } else {
            vcs_repo_learn_names(&repo);
            for (i++; i < argc; i++) {
                char rp[1024];
                vcs_path_from_local(argv[i], rp, sizeof(rp));
                if (!vcs_repo_merge_resolve(&repo, rp, side)) rc = 1;
            }
        }

    } else if (strcmp(cmd, "diff") == 0) {
        rc = cmd_diff(&repo, argc, argv, i);

    } else if (strcmp(cmd, "show") == 0) {
        rc = cmd_show(&repo, argc, argv, i);

    } else if (strcmp(cmd, "push") == 0 || strcmp(cmd, "pull") == 0) {
        const char *url = (i < argc && is_url(argv[i])) ? argv[i++] : NULL;
        const char *branch = (i < argc) ? argv[i] : NULL;
        char repo_name[256], shown_url[AC_MAX_URL + 256];
        int tries, remember = 0, ok = 0;
        if (!vcs_remote_resolve(repo.amivcs_dir, url, cfg, repo_name, sizeof(repo_name))) rc = 1;
        else if (!http_init()) { ui_notify(UI_ERROR, "amivcs: network initialisation failed"); rc = 1; }
        else {
            snprintf(shown_url, sizeof(shown_url), "%s/%s", cfg->server, repo_name);
            for (tries = 0; tries < 3 && !ok; tries++) {
                if (!get_credentials(cfg, shown_url, tries > 0, &remember)) break;
                if (strcmp(cmd, "push") == 0) ok = vcs_remote_push(&repo, cfg, repo_name, url != NULL);
                else {
                    char *b = branch ? vcs_repo_branch_name(&repo, branch) : NULL;
                    ok = vcs_remote_pull(&repo, cfg, repo_name, b);
                    free(b);
                }
                if (!ok && !vcs_remote_auth_failed()) break;
            }
            if (ok) {
                if (url) vcs_repo_config_set(repo.amivcs_dir, "remote", shown_url);
                store_credentials(repo.amivcs_dir, cfg, remember);
            }
            rc = !ok;
            http_cleanup();
        }

    } else if (strcmp(cmd, "remote") == 0) {
        if (i < argc) {
            char *set_argv[3];
            set_argv[0] = "config"; set_argv[1] = "remote"; set_argv[2] = argv[i];
            rc = cmd_config(3, set_argv, 1, repo.amivcs_dir);
        } else {
            VcsRepoConfig rcfg;
            vcs_repo_config_load(repo.amivcs_dir, &rcfg);
            if (rcfg.remote[0]) printf("%s\n", rcfg.remote);
            else { ui_notify(UI_INFO, "no remote set (amivcs remote <url>)"); rc = 1; }
        }

    } else if (strcmp(cmd, "config") == 0) {
        rc = cmd_config(argc, argv, i, repo.amivcs_dir);

    } else if (strcmp(cmd, "gc") == 0) {
        int count = 0;
        if (!vcs_pack_gc(&repo.store, &count)) rc = 1;
        else printf("packed %d object(s)\n", count);

    } else if (strcmp(cmd, "cat-file") == 0) {
        VcsOid oid;
        if (i >= argc) { ui_notify(UI_ERROR, "amivcs: cat-file requires <oid>"); rc = 1; }
        else if (!vcs_oid_from_hex(argv[i], &oid)) { ui_notify(UI_ERROR, "amivcs: malformed oid '%s'", argv[i]); rc = 1; }
        else {
            VcsObjType type;
            void *data;
            long len;
            if (!vcsobjstore_read(&repo.store, &oid, &type, &data, &len)) { ui_notify(UI_ERROR, "amivcs: no such object"); rc = 1; }
            else {
                const char *tname = type == VCS_OBJ_BLOB ? "blob" : type == VCS_OBJ_TREE ? "tree" : "commit";
                printf("%s %ld\n", tname, len);
                free(data);
            }
        }

    } else {
        ui_notify(UI_ERROR, "amivcs: unknown command '%s'", cmd);
        if (locked) vcs_repo_unlock(repo.amivcs_dir);
        usage("amivcs");
    }

    if (locked) vcs_repo_unlock(repo.amivcs_dir);

    /* a changing command reports the resulting state too: one process
     * instead of two for the IDE */
    if (g_pc && is_writing(cmd) && strcmp(cmd, "gc") != 0) pc_status(&repo);
    if (g_pc && (strcmp(cmd, "branch") == 0 && i < argc)) pc_branches(&repo);
    return rc;
}
