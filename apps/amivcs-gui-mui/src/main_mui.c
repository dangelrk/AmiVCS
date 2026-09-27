/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * main_mui.c — amivcs-gui entry point (Amiga/MUI).
 *
 * Deliberately simpler than a full connect dialog for v1 (matching
 * amicloud-commander-mui's main_mui.c): no server/user/pass are required
 * up front at all -- AMIVCS's local commands (status/add/commit/branch/
 * switch/merge/log) never touch the network. Clone/Push/Pull ask for
 * server/user/pass on first use instead (gui.c's ensure_remote_configured()),
 * so a purely local AMIVCS workflow never needs ~/.amivcsrc/ENV:amivcs.cfg
 * at all.
 */
#include <libraries/mui.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "amicloud.h"
#include "config.h"
#include "http.h"
#include "gui.h"
#include "mui_stubs.h"
#include "progress.h"
#include "wbstartup.h"

#include <stdio.h>
#include <string.h>

struct Library       *MUIMasterBase;
struct IntuitionBase *IntuitionBase;

extern int ac_verbose;
extern int ac_errors_to_stderr;

long __stack = 65536;
const char amiga_ver[] = "$VER: amivcs-gui " AC_VERSION " (13.07.2026)";

/* Debug toggle (-v / DEBUG tooltype) and repo path (a plain CLI argument,
 * or a REPO=<path> tooltype) -- read together since both need the same
 * WBStartup/icon.library dance for a Workbench launch (wbstartup.h,
 * factored out of here and amicloud-commander-mui/amicloud-sync-cx,
 * which each had their own copy of exactly this). repo_path is left
 * untouched (caller pre-clears it) if neither source supplies one --
 * gui_run() then falls back to the current directory, exactly like a
 * bare CLI launch. */
static void configure_startup(int argc, char *argv[], char *repo_path, size_t repo_path_sz)
{
    int debug;

    if (argc > 0) {
        int i;
        debug = 0;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-v") == 0) { debug = 1; continue; }
            if (!repo_path[0]) {
                strncpy(repo_path, argv[i], repo_path_sz - 1);
                repo_path[repo_path_sz - 1] = '\0';
            }
        }
    } else {
        debug = ac_wb_tooltype_present(argc, argv, "DEBUG");
        ac_wb_tooltype(argc, argv, "REPO", repo_path, repo_path_sz);
    }

    ac_verbose          = debug;
    ac_errors_to_stderr = debug;
    ac_console_progress = 0; /* windowed GUI app -- never print \r status lines */
}

static int open_libs(void)
{
    MUIMasterBase = OpenLibrary("muimaster.library", 19);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    return MUIMasterBase && IntuitionBase;
}

static void close_libs(void)
{
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
    IntuitionBase = NULL;
    MUIMasterBase = NULL;
}

/* A Workbench-launched GUI program has no console: without a requester a
 * startup failure would just make it vanish. Uses MUI when available,
 * falls back to a plain Intuition requester (e.g. MUI not installed) and
 * additionally prints to stderr for a Shell launch. Call after
 * open_libs() -- works with whatever that managed to open. */
static void startup_error(int from_shell, const char *text)
{
    if (from_shell) fprintf(stderr, "amivcs-gui: %s\n", text);

    if (MUIMasterBase && IntuitionBase) {
        MUI_Request(NULL, NULL, 0, "amivcs-gui", "_OK", "%s", (char *)text);
    } else if (IntuitionBase) {
        struct EasyStruct es;
        ULONG args[1];
        es.es_StructSize   = sizeof(es);
        es.es_Flags        = 0;
        es.es_Title        = (UBYTE *)"amivcs-gui";
        es.es_TextFormat   = (UBYTE *)"%s";
        es.es_GadgetFormat = (UBYTE *)"OK";
        args[0] = (ULONG)text;
        EasyRequestArgs(NULL, &es, NULL, args);
    }
}

int main(int argc, char *argv[])
{
    AcConfig cfg;
    char repo_path[1024];
    int rc;

    repo_path[0] = '\0';
    configure_startup(argc, argv, repo_path, sizeof(repo_path));

    memset(&cfg, 0, sizeof(cfg));
    cfg.basepath[0] = '/';
    cfg.verify_ssl  = 1;

    /* Own config file, fully separate from amicloud's -- same
     * ENV:/current-dir fallback the CLI uses (apps/amivcs/src/main.c). */
    if (!config_load("ENV:amivcs.cfg", &cfg))
        config_load("amivcs.cfg", &cfg);
    {
        size_t len = strlen(cfg.server);
        if (len > 0 && cfg.server[len - 1] == '/') cfg.server[len - 1] = '\0';
    }

    if (!open_libs()) {
        startup_error(argc > 0, "amivcs-gui needs muimaster.library V19 "
                                "(MUI 3.8) or newer.");
        close_libs();
        return 20;
    }

    /* No http_init() here: like git, local work needs no network --
     * gui.c opens it on the first clone/push/pull (ensure_network()).
     * http_cleanup() is a no-op if that never happened. */
    rc = gui_run(&cfg, repo_path);

    http_cleanup();
    close_libs();
    return rc;
}
