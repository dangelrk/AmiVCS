/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * amivcs-prefs -- Preferences editor for amivcs.cfg (amivcs CLI,
 * amivcs-gui-mui/-qt). GadTools window built on core/common's
 * prefsgui.h toolbox (see repo plan "Drei getrennte Prefs-Programme");
 * own program, no dependency on amicloud-prefs or amicloudsync-prefs --
 * AMIVCS and AmiCloud share no server, no credentials (see
 * config-examples/README.md and docs/adr/09_ADR_VCS_Design.md).
 *
 * Classic Workbench-Prefs USE/SAVE/CANCEL semantics (bindend, see the
 * plan): Benutzen writes only ENV:amivcs.cfg (live now, gone after a
 * reboot); Speichern writes ENVARC:amivcs.cfg AND ENV:amivcs.cfg (live
 * now, and permanent); Abbrechen writes nothing.
 *
 * "Benutzer" doubles as the commit-author identity ("amivcs log") --
 * see amivcs/src/main.c -- purely-local commands (init/add/commit/log/
 * branch/merge/...) don't need server/pass at all, only clone/push/pull
 * do; this editor still offers all four fields together in one place.
 */
#include "amicloud.h"
#include "config.h"
#include "prefsgui.h"

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

long __stack = 65536;
const char amiga_ver[] = "$VER: AMIVCSPrefs " AC_VERSION " (27.09.2026)";

#define GID_SERVER    1
#define GID_USER      2
#define GID_PASS      3
#define GID_VERIFYSSL 4
#define GID_STATUS    5
#define GID_USE       6
#define GID_SAVE      7
#define GID_CANCEL    8

#define PREFS_ENV    "ENV:amivcs.cfg"
#define PREFS_ENVARC "ENVARC:amivcs.cfg"

int main(int argc, char *argv[])
{
    AcConfig cfg;
    struct Gadget *glist, *gad;
    struct Gadget *g_server, *g_user, *g_pass, *g_verifyssl;
    struct Window *win;
    PrefsLayout lay;
    WORD winw, winh, topborder;
    int running;
    const char *notice;

    memset(&cfg, 0, sizeof(cfg));
    /* ENV: first -- the CURRENT/live copy (see the repo's USE/SAVE/
     * CANCEL semantics). NEVER config_default_path()'s own fallback --
     * that resolves to a bare CWD-relative "amivcs.cfg", a CLI-tool
     * convention for a project-local override, not a Prefs editor's
     * durable store. */
    if (!config_load(PREFS_ENV, &cfg)) config_load(PREFS_ENVARC, &cfg);
    notice = prefsgui_files_differ(PREFS_ENV, PREFS_ENVARC)
           ? "ENV:/ENVARC: unterscheiden sich (angezeigt: ENV:)"
           : "";

    if (!prefsgui_screen_open()) return 1;

    topborder = prefsgui_topborder();
    winw = 460;
    winh = topborder + 8 + 5 * 26 + 26 + 8;

    glist = NULL;
    gad = CreateContext(&glist);
    if (!gad) { prefsgui_screen_close(); return 1; }

    prefsgui_layout_init(&lay, gad, topborder + 8, 180, winw - 180 - 8, 20, 6);

    g_server = prefsgui_field_string(&lay, "Server", GID_SERVER, cfg.server, sizeof(cfg.server) - 1);
    if (!g_server) goto fail;

    g_user = prefsgui_field_string(&lay, "Benutzer (= Autor)", GID_USER, cfg.username, sizeof(cfg.username) - 1);
    if (!g_user) goto fail;

    g_pass = prefsgui_field_string(&lay, "Passwort", GID_PASS, cfg.password, sizeof(cfg.password) - 1);
    if (!g_pass) goto fail;

    g_verifyssl = prefsgui_field_checkbox(&lay, "Zertifikat pruefen", GID_VERIFYSSL, cfg.verify_ssl);
    if (!g_verifyssl) goto fail;

    gad = prefsgui_status_line(&lay, 8, winw - 16, GID_STATUS, notice);
    if (!gad) goto fail;

    gad = prefsgui_use_save_cancel_row(&lay, 8, winw - 16, GID_USE, GID_SAVE, GID_CANCEL);
    if (!gad) goto fail;

    win = OpenWindowTags(NULL,
        WA_Left,      60,
        WA_Top,       40,
        WA_Width,     winw,
        WA_Height,    winh,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP,
        WA_Gadgets,   (ULONG)glist,
        WA_Title,     (ULONG)"AMIVCS-Einstellungen",
        WA_PubScreen, (ULONG)prefsgui_screen(),
        TAG_DONE);
    if (!win) goto fail;

    GT_RefreshWindow(win, NULL);

    running = 1;
    while (running) {
        UWORD act = prefsgui_wait_action(win, NULL);

        if (act == GID_VERIFYSSL) {
            /* Tracked app-side, not read back via GT_GetGadgetAttrs --
             * see prefsgui_set_checkbox()'s doc comment in prefsgui.h. */
            cfg.verify_ssl = !cfg.verify_ssl;
            prefsgui_set_checkbox(win, g_verifyssl, cfg.verify_ssl);
        } else if (act == GID_USE || act == GID_SAVE) {
            prefsgui_get_string(g_server, cfg.server,   sizeof(cfg.server));
            prefsgui_get_string(g_user,   cfg.username, sizeof(cfg.username));
            prefsgui_get_string(g_pass,   cfg.password, sizeof(cfg.password));

            config_save(PREFS_ENV, &cfg, 0);
            if (act == GID_SAVE) config_save(PREFS_ENVARC, &cfg, 0);
            running = 0;
        } else if (act == GID_CANCEL || act == PREFSGUI_ACT_CLOSE) {
            running = 0;
        }
    }

    CloseWindow(win);
    FreeGadgets(glist);
    prefsgui_screen_close();
    return 0;

fail:
    FreeGadgets(glist);
    prefsgui_screen_close();
    return 1;
}
