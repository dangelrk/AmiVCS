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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef PLATFORM_AMIGA
long __stack = 65536;
const char amiga_ver[] = "$VER: amivcs " AC_VERSION " (27.09.2026)";
#endif

/* Own config file, fully separate from amicloud's (plan section 2's
 * separation table): "user=" doubles as the commit author identity --
 * AcConfig already has the field, no need to extend libcloudcore for it
 * (plan section 5). server/pass are unused until remote.h's clone/push/
 * pull land. Basename "amivcs" -> ENV:amivcs.cfg/~/.amivcsrc, see
 * config_default_path(). */

/* remote.c's clone/push/pull now call ui_progress() too (for real cancel
 * support, wired for the GUI frontends) -- the console CLI has no cancel
 * mechanism and already prints its own "\rLabel: N%%..." lines via
 * progress_line(), so it installs a no-op progress hook rather than the
 * ui_hooks.h default (which would draw ITS OWN "\r%-24.24s..." status
 * line on top, double-printing). notify/confirm/overwrite are left NULL
 * so ui_set_hooks() keeps their existing console defaults. */
static int noop_progress(long done, long total, const char *name)
{
    (void)done; (void)total; (void)name;
    return 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "amivcs v" AC_VERSION " -- minimal, Amiga-optimised version control\n"
        "Usage: %s [--author <name>] [-c <config-file>] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  init                 Create a new repository in the current directory\n"
        "  status               Show staged/unstaged changes since the last commit\n"
        "  add <path>...        Stage file(s)/directory(ies) for the next commit\n"
        "  add -A               Stage all changes (new/modified/deleted)\n"
        "  remove <path>...     Unstage path(s) (working tree is left alone)\n"
        "  commit -m <message> [-a]\n"
        "                       Commit what's staged; -a stages everything first\n"
        "  log                  Show commit history of the current branch\n"
        "  branch [<name>]      List branches, or create a new one at HEAD\n"
        "  switch <branch>      Change the current branch and update the working tree\n"
        "  checkout             Re-materialize HEAD's commit into the working tree\n"
        "  merge <branch>       Merge <branch> into the current branch\n"
        "  gc                   Bundle loose objects into a pack\n"
        "  cat-file <oid>       Print a stored object's type and size (debug)\n"
        "  clone <repo> [dir]   Clone <repo> from the configured server into [dir]\n"
        "  push [<repo>]        Push the current branch to the server\n"
        "  pull [<branch>]      Fetch and merge <branch> (default: current) from the server\n"
        "  version              Print the version\n"
        "\n"
        "Must be run from the repository root (no parent-directory search).\n"
        "clone/push/pull need server=/user=/pass= in the config file (default:\n"
        "ENV:amivcs.cfg on Amiga, ~/.amivcsrc elsewhere).\n",
        prog);
    exit(1);
}

int main(int argc, char **argv)
{
    AcConfig cfg;
    const char *opt_author = NULL;
    const char *opt_cfgfile = NULL;
    int i;
    const char *cmd;
    VcsRepo repo;
    int rc = 0;

    {
        UiHooks hooks;
        memset(&hooks, 0, sizeof(hooks));
        hooks.progress = noop_progress;
        ui_set_hooks(&hooks);
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.basepath[0] = '/';
    cfg.basepath[1] = '\0';
    cfg.verify_ssl  = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) { opt_author = argv[++i]; continue; }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) { opt_cfgfile = argv[++i]; continue; }
        if (argv[i][0] == '-' && strcmp(argv[i], "--help") != 0) continue; /* unrecognised leading option: ignore, let dispatch fail clearly */
        break;
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

    if (i >= argc) { fprintf(stderr, "amivcs: no command given\n"); usage(argv[0]); }
    cmd = argv[i++];

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--help") == 0) {
        printf("AMIVCS v" AC_VERSION "\n");
        return 0;
    }

    if (strcmp(cmd, "init") == 0) {
        if (!vcs_repo_init(".")) {
            fprintf(stderr, "amivcs: init failed (already a repository here?)\n");
            return 1;
        }
        printf("Initialized empty AMIVCS repository in .amivcs/\n");
        return 0;
    }

    if (strcmp(cmd, "clone") == 0) {
        const char *repo_name, *local_dir;
        if (i >= argc) { fprintf(stderr, "amivcs: clone requires <repo> [dir]\n"); return 1; }
        repo_name = argv[i++];
        local_dir = (i < argc) ? argv[i] : repo_name;
        if (!cfg.server[0] || !cfg.username[0] || !cfg.password[0]) {
            fprintf(stderr, "amivcs: clone requires server=/user=/pass= in the config file\n");
            return 1;
        }
        if (!http_init()) { fprintf(stderr, "amivcs: network initialisation failed\n"); return 1; }
        rc = !vcs_remote_clone(&cfg, repo_name, local_dir);
        http_cleanup();
        return rc;
    }

    if (!vcs_repo_open(&repo, ".")) {
        fprintf(stderr, "amivcs: not an AMIVCS repository (no .amivcs/ here -- run from the repo root)\n");
        return 1;
    }

    if (strcmp(cmd, "status") == 0) {
        rc = !vcs_repo_status(&repo);

    } else if (strcmp(cmd, "commit") == 0) {
        const char *message = NULL;
        int stage_all = 0;
        int j = i;
        while (j < argc) {
            if (strcmp(argv[j], "-m") == 0 && j + 1 < argc) { message = argv[j + 1]; j += 2; }
            else if (strcmp(argv[j], "-a") == 0 || strcmp(argv[j], "--all") == 0) { stage_all = 1; j++; }
            else j++;
        }
        if (!message) { fprintf(stderr, "amivcs: commit requires -m <message>\n"); rc = 1; }
        else {
            VcsOid oid;
            if (!vcs_repo_commit(&repo, cfg.username, message, stage_all, &oid)) rc = 1;
            else {
                char hex[VCS_OID_HEX];
                vcs_oid_to_hex(&oid, hex);
                printf("committed %s\n", hex);
            }
        }

    } else if (strcmp(cmd, "add") == 0) {
        if (i >= argc) { fprintf(stderr, "amivcs: add requires <path>... or -A\n"); rc = 1; }
        else if (strcmp(argv[i], "-A") == 0) {
            rc = !vcs_index_add_all(&repo);
        } else {
            for (; i < argc; i++) {
                if (!vcs_index_add(&repo, argv[i])) {
                    fprintf(stderr, "amivcs: could not add '%s'\n", argv[i]);
                    rc = 1;
                }
            }
        }

    } else if (strcmp(cmd, "remove") == 0) {
        if (i >= argc) { fprintf(stderr, "amivcs: remove requires <path>...\n"); rc = 1; }
        else {
            for (; i < argc; i++) {
                if (!vcs_index_remove(&repo, argv[i])) {
                    fprintf(stderr, "amivcs: could not remove '%s'\n", argv[i]);
                    rc = 1;
                }
            }
        }

    } else if (strcmp(cmd, "log") == 0) {
        rc = !vcs_repo_log(&repo);

    } else if (strcmp(cmd, "branch") == 0) {
        const char *name = (i < argc) ? argv[i] : NULL;
        rc = !vcs_repo_branch(&repo, name);

    } else if (strcmp(cmd, "switch") == 0) {
        if (i >= argc) { fprintf(stderr, "amivcs: switch requires <branch>\n"); rc = 1; }
        else rc = !vcs_repo_switch(&repo, argv[i]);

    } else if (strcmp(cmd, "checkout") == 0) {
        rc = !vcs_repo_checkout(&repo);

    } else if (strcmp(cmd, "merge") == 0) {
        if (i >= argc) { fprintf(stderr, "amivcs: merge requires <branch>\n"); rc = 1; }
        else rc = !vcs_repo_merge(&repo, argv[i]);

    } else if (strcmp(cmd, "push") == 0 || strcmp(cmd, "pull") == 0) {
        const char *arg = (i < argc) ? argv[i] : NULL;
        if (!cfg.server[0] || !cfg.username[0] || !cfg.password[0]) {
            fprintf(stderr, "amivcs: %s requires server=/user=/pass= in the config file\n", cmd);
            rc = 1;
        } else if (!http_init()) {
            fprintf(stderr, "amivcs: network initialisation failed\n");
            rc = 1;
        } else {
            if (strcmp(cmd, "push") == 0) rc = !vcs_remote_push(&repo, &cfg, arg);
            else                          rc = !vcs_remote_pull(&repo, &cfg, NULL, arg);
            http_cleanup();
        }

    } else if (strcmp(cmd, "gc") == 0) {
        int count = 0;
        if (!vcs_pack_gc(&repo.store, &count)) rc = 1;
        else printf("packed %d object(s)\n", count);

    } else if (strcmp(cmd, "cat-file") == 0) {
        VcsOid oid;
        if (i >= argc) { fprintf(stderr, "amivcs: cat-file requires <oid>\n"); rc = 1; }
        else if (!vcs_oid_from_hex(argv[i], &oid)) { fprintf(stderr, "amivcs: malformed oid '%s'\n", argv[i]); rc = 1; }
        else {
            VcsObjType type;
            void *data;
            long len;
            if (!vcsobjstore_read(&repo.store, &oid, &type, &data, &len)) { fprintf(stderr, "amivcs: no such object\n"); rc = 1; }
            else {
                const char *tname = type == VCS_OBJ_BLOB ? "blob" : type == VCS_OBJ_TREE ? "tree" : "commit";
                printf("%s %ld\n", tname, len);
                free(data);
            }
        }

    } else {
        fprintf(stderr, "amivcs: unknown command '%s'\n", cmd);
        usage(argv[0]);
    }

    return rc;
}
