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
#include "pack.h"
#include "remote.h"
#include "http.h"
#include "ui_hooks.h"
#include "charset.h"
#include "vcsname.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>

#ifdef PLATFORM_AMIGA
long __stack = 65536;
const char amiga_ver[] = "$VER: amivcs " AC_VERSION " (" AC_DATE ")";
#endif

/* Own config file, fully separate from amicloud's: "user=" doubles as the
 * commit author. Basename "amivcs" -> ENV:amivcs.cfg / ~/.amivcsrc, see
 * config_default_path(). */

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

static int pc_status(VcsRepo *repo)
{
    VcsStatus st;
    if (!vcs_repo_status_raw(repo, &st)) return 0;
    pc_head(repo);
    pc_changes(&st.staged, "S");
    pc_changes(&st.unstaged, "U");
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

static void usage(const char *prog)
{
    fprintf(stderr,
        "amivcs v" AC_VERSION " -- minimal, Amiga-optimised version control\n"
        "Usage: %s [options] <command> [args]\n"
        "\n"
        "Options:\n"
        "  --author <name>      Commit author (default: user= from the config file)\n"
        "  -c <config-file>     Config file (default: ENV:amivcs.cfg / ~/.amivcsrc)\n"
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
        "  gc                   Bundle loose objects into a pack\n"
        "  cat-file <oid>       Print a stored object's type and size (debug)\n"
        "  clone <repo> [dir]   Clone <repo> from the configured server into [dir]\n"
        "  push [<repo>]        Push the current branch to the server\n"
        "  pull [<branch>]      Fetch and merge <branch> (default: current) from the server\n"
        "  version              Print the version\n"
        "\n"
        "Must be run from the repository root (no parent-directory search).\n"
        "clone/push/pull need server=/user=/pass= in the config file.\n",
        prog);
    exit(1);
}

/* commands that change the repository or the working tree take the lock */
static int is_writing(const char *cmd)
{
    static const char *const w[] = { "add", "unstage", "remove", "commit", "switch", "checkout",
                                     "merge", "pull", "gc", NULL };
    int k;
    for (k = 0; w[k]; k++) if (strcmp(cmd, w[k]) == 0) return 1;
    return 0;
}

static int run(int argc, char **argv, AcConfig *cfg);

int main(int argc, char **argv)
{
    AcConfig cfg;
    const char *opt_author = NULL, *opt_cfgfile = NULL, *opt_pc = NULL, *opt_args = NULL;
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
        if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) { opt_author = argv[++i]; continue; }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) { opt_cfgfile = argv[++i]; continue; }
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

    if (opt_cfgfile) {
        config_load(opt_cfgfile, &cfg);
    } else {
        char primary[AC_MAX_PATH], fallback[AC_MAX_PATH];
        config_default_path("amivcs", primary, sizeof(primary), fallback, sizeof(fallback));
        if (!config_load(primary, &cfg) && fallback[0]) config_load(fallback, &cfg);
    }
    if (opt_author) strncpy(cfg.username, opt_author, sizeof(cfg.username) - 1);
    {
        size_t len = strlen(cfg.server);
        if (len > 0 && cfg.server[len - 1] == '/') cfg.server[len - 1] = '\0';
    }

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
        const char *repo_name, *local_dir;
        if (i >= argc) { ui_notify(UI_ERROR, "amivcs: clone requires <repo> [dir]"); return 1; }
        repo_name = argv[i++];
        local_dir = (i < argc) ? argv[i] : repo_name;
        if (!cfg->server[0] || !cfg->username[0] || !cfg->password[0]) {
            ui_notify(UI_ERROR, "amivcs: clone requires server=/user=/pass= in the config file");
            return 1;
        }
        if (!http_init()) { ui_notify(UI_ERROR, "amivcs: network initialisation failed"); return 1; }
        rc = !vcs_remote_clone(cfg, repo_name, local_dir);
        http_cleanup();
        return rc;
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
        if (!rc && (!message || !message[0])) { ui_notify(UI_ERROR, "amivcs: commit requires -m <message> or -F <file>"); rc = 1; }
        if (!rc) {
            VcsOid oid;
            char *author = vcs_text_from_local(cfg->username);
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
        if (i >= argc) { ui_notify(UI_ERROR, "amivcs: merge requires <branch>"); rc = 1; }
        else {
            char *b = vcs_repo_branch_name(&repo, argv[i]);
            rc = !b || !vcs_repo_merge(&repo, b);
            free(b);
        }

    } else if (strcmp(cmd, "push") == 0 || strcmp(cmd, "pull") == 0) {
        const char *arg = (i < argc) ? argv[i] : NULL;
        if (!cfg->server[0] || !cfg->username[0] || !cfg->password[0]) {
            ui_notify(UI_ERROR, "amivcs: %s requires server=/user=/pass= in the config file", cmd);
            rc = 1;
        } else if (!http_init()) {
            ui_notify(UI_ERROR, "amivcs: network initialisation failed");
            rc = 1;
        } else {
            if (strcmp(cmd, "push") == 0) rc = !vcs_remote_push(&repo, cfg, arg);
            else {
                char *b = arg ? vcs_repo_branch_name(&repo, arg) : NULL;
                rc = !vcs_remote_pull(&repo, cfg, NULL, b);
                free(b);
            }
            http_cleanup();
        }

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
