/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * AmiVCSConfig -- settings for AmiVCS (docs/design/remotes-plan.md):
 *
 *   - defaults for every repository: name and e-mail of the commit
 *     author (ENV:/ENVARC:AmiVCS/config, see vcsconfig.h);
 *   - one repository: its remote URL, the user for that server, an own
 *     name/e-mail (empty = the defaults), and forgetting a remembered
 *     password (.amivcs/config of that repository).
 *
 * The repository is the directory given as argument, the REPO= tooltype
 * of the icon, or the current directory. amivcs-gui and the AmigaIDE
 * start it with the repository as argument.
 *
 * GadTools window on core/common's prefsgui.h toolbox like the other
 * prefs programs. Save writes both files, Cancel nothing.
 */
#include "amicloud.h"
#include "prefsgui.h"
#include "winpos.h"
#include "vcsconfig.h"
#include "repo.h"      /* VCS_PROJECT_FILE */
#include "wbstartup.h"

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <utility/tagitem.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

long __stack = 65536;
const char amiga_ver[] = "$VER: AmiVCSConfig " AC_VERSION " (" AC_DATE ")";

#define GID_GNAME   1
#define GID_GEMAIL  2
#define GID_REMOTE  3
#define GID_USER    4
#define GID_RNAME   5
#define GID_REMAIL  6
#define GID_FORGET  7
#define GID_STATUS  8
#define GID_SAVE    9
#define GID_CANCEL  10
#define GID_HEAD1   11
#define GID_HEAD2   12
#define GID_LONGNAMES 13

#define WINPOS_VAR "AmiVCS/Windows/AmiVCSConfig"

/* Speichern / Abbrechen -- these settings are files, there is no
 * separate "use for now" like ENV: prefs have. */
static struct Gadget *save_cancel_row(PrefsLayout *lay, WORD x, WORD total_w)
{
    struct NewGadget ng;
    struct Gadget *gad;
    WORD bw = (total_w - lay->gap) / 2;

    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr   = prefsgui_font();
    ng.ng_VisualInfo = prefsgui_vi();
    ng.ng_TopEdge    = lay->y;
    ng.ng_Width      = bw;
    ng.ng_Height     = lay->row_h;
    ng.ng_Flags      = PLACETEXT_IN;

    ng.ng_LeftEdge = x;
    ng.ng_GadgetText = (CONST_STRPTR)"Speichern";
    ng.ng_GadgetID = GID_SAVE;
    gad = CreateGadget(BUTTON_KIND, lay->context, &ng, TAG_DONE);
    if (!gad) return NULL;
    lay->context = gad;

    ng.ng_LeftEdge = x + bw + lay->gap;
    ng.ng_GadgetText = (CONST_STRPTR)"Abbrechen";
    ng.ng_GadgetID = GID_CANCEL;
    gad = CreateGadget(BUTTON_KIND, lay->context, &ng, TAG_DONE);
    if (!gad) return NULL;
    lay->context = gad;
    lay->y += lay->row_h + lay->gap;
    return gad;
}

/* The repository: argument, REPO= tooltype, or the current directory.
 * *amivcs_dir is empty if there is no repository there. */
static void find_repository(int argc, char *argv[], char *root, size_t rsz,
                            char *amivcs_dir, size_t asz)
{
    BPTR lock;

    root[0] = '\0';
    if (argc > 1) {
        strncpy(root, argv[1], rsz - 1);
        root[rsz - 1] = '\0';
    } else if (argc == 0) {
        ac_wb_tooltype(argc, argv, "REPO", root, rsz);
    }
    if (!root[0]) {
        BPTR cd = Lock((STRPTR)"", ACCESS_READ);
        if (cd) {
            NameFromLock(cd, (STRPTR)root, (LONG)rsz);
            UnLock(cd);
        }
    }
    strncpy(amivcs_dir, root, asz - 1);
    amivcs_dir[asz - 1] = '\0';
    AddPart((STRPTR)amivcs_dir, (STRPTR)".amivcs", (ULONG)asz);
    lock = Lock((STRPTR)amivcs_dir, ACCESS_READ);
    if (lock) UnLock(lock);
    else amivcs_dir[0] = '\0';
}

/* A string field without leading/trailing blanks. */
static void get_field(struct Gadget *g, char *out, size_t sz)
{
    size_t n;
    char *p;
    prefsgui_get_string(g, out, sz);
    p = out;
    while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
    n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = '\0';
}

int main(int argc, char *argv[])
{
    char root[512], amivcs_dir[600], head2[600];
    char gname[128], gemail[128];
    VcsRepoConfig rc;
    struct Gadget *glist, *gad, *g_status;
    struct Gadget *g_gname, *g_gemail, *g_remote = NULL, *g_user = NULL;
    struct Gadget *g_rname = NULL, *g_remail = NULL, *g_forget = NULL;
    struct Window *win;
    PrefsLayout lay;
    WORD winw, winh, topborder, rows, left = 60, top = 40;
    int running, forget = 0, have_repo;
    struct Gadget *g_longnames = NULL;
    char projfile[600], lnval[16];
    int longnames = 0, longnames_before = 0;

    find_repository(argc, argv, root, sizeof(root), amivcs_dir, sizeof(amivcs_dir));
    have_repo = amivcs_dir[0] != '\0';
    vcs_global_get("name", gname, sizeof(gname));
    vcs_global_get("email", gemail, sizeof(gemail));
    memset(&rc, 0, sizeof(rc));
    if (have_repo) vcs_repo_config_load(amivcs_dir, &rc);
    /* the project's long-name switch: versioned, in the working tree */
    strncpy(projfile, root, sizeof(projfile) - 1);
    projfile[sizeof(projfile) - 1] = '\0';
    AddPart((STRPTR)projfile, (STRPTR)VCS_PROJECT_FILE, (ULONG)sizeof(projfile));
    if (have_repo && vcs_config_file_get(projfile, "longnames", lnval, sizeof(lnval)))
        longnames = strcmp(lnval, "1") == 0 || strcmp(lnval, "on") == 0
                 || strcmp(lnval, "yes") == 0 || strcmp(lnval, "true") == 0;
    longnames_before = longnames;

    if (!prefsgui_screen_open()) return 1;

    topborder = prefsgui_topborder();
    winw = 520;
    /* heading, 2 fields, heading, repository rows, status, buttons */
    rows = 1 + 2 + 1 + (have_repo ? (rc.pass[0] ? 6 : 5) : 0) + 1 + 1;
    winh = topborder + 8 + rows * 26 + 8;

    glist = NULL;
    gad = CreateContext(&glist);
    if (!gad) { prefsgui_screen_close(); return 1; }

    prefsgui_layout_init(&lay, gad, topborder + 8, 190, winw - 190 - 8, 20, 6);

    if (!prefsgui_status_line(&lay, 8, winw - 16, GID_HEAD1, "Vorgaben f\374r alle Repositories:")) goto fail;
    g_gname = prefsgui_field_string(&lay, "Name", GID_GNAME, gname, sizeof(gname) - 1);
    if (!g_gname) goto fail;
    g_gemail = prefsgui_field_string(&lay, "E-Mail", GID_GEMAIL, gemail, sizeof(gemail) - 1);
    if (!g_gemail) goto fail;

    if (have_repo) snprintf(head2, sizeof(head2), "Repository %s:", root);
    else snprintf(head2, sizeof(head2), "Kein Repository in %s", root[0] ? root : "diesem Verzeichnis");
    if (!prefsgui_status_line(&lay, 8, winw - 16, GID_HEAD2, head2)) goto fail;

    if (have_repo) {
        g_remote = prefsgui_field_string(&lay, "Remote-URL", GID_REMOTE, rc.remote, sizeof(rc.remote) - 1);
        if (!g_remote) goto fail;
        g_user = prefsgui_field_string(&lay, "Benutzer (Server)", GID_USER, rc.user, sizeof(rc.user) - 1);
        if (!g_user) goto fail;
        g_rname = prefsgui_field_string(&lay, "Name (leer=Vorgabe)", GID_RNAME, rc.name, sizeof(rc.name) - 1);
        if (!g_rname) goto fail;
        g_remail = prefsgui_field_string(&lay, "E-Mail (leer=Vorgabe)", GID_REMAIL, rc.email, sizeof(rc.email) - 1);
        if (!g_remail) goto fail;
        if (rc.pass[0]) {
            g_forget = prefsgui_field_checkbox(&lay, "Passwort vergessen", GID_FORGET, 0);
            if (!g_forget) goto fail;
        }
        /* FFS keeps 30 characters -- without this a commit refuses longer
         * new names (repo.h VCS_PROJECT_FILE) */
        g_longnames = prefsgui_field_checkbox(&lay, "Lange Dateinamen", GID_LONGNAMES, longnames);
        if (!g_longnames) goto fail;
    }

    g_status = prefsgui_status_line(&lay, 8, winw - 16, GID_STATUS,
                                    have_repo ? (rc.pass[0] ? "Ein Passwort ist gemerkt." : "")
                                              : "Pfad als Argument oder Merkmal REPO= angeben.");
    if (!g_status) goto fail;
    if (!save_cancel_row(&lay, 8, winw - 16)) goto fail;

    ac_winpos_load(WINPOS_VAR, prefsgui_screen(), winw, winh, &left, &top);
    win = OpenWindowTags(NULL,
        WA_Left,      left,
        WA_Top,       top,
        WA_Width,     winw,
        WA_Height,    winh,
        WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_GADGETUP,
        WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP,
        WA_Gadgets,   (ULONG)glist,
        WA_Title,     (ULONG)"AmiVCS-Einstellungen",
        WA_PubScreen, (ULONG)prefsgui_screen(),
        TAG_DONE);
    if (!win) goto fail;

    GT_RefreshWindow(win, NULL);

    running = 1;
    while (running) {
        UWORD act = prefsgui_wait_action(win, NULL);

        if (act == GID_FORGET) {
            /* tracked here, see prefsgui_set_checkbox() */
            forget = !forget;
            prefsgui_set_checkbox(win, g_forget, forget);
        } else if (act == GID_LONGNAMES) {
            longnames = !longnames;
            prefsgui_set_checkbox(win, g_longnames, longnames);
            GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text,
                              (ULONG)(longnames ? "Lange Namen: nur f\374r Volumes mit langen Namen (nicht FFS)."
                                                : "Amiga-Modus: neue Namen h\366chstens 30 Zeichen."), TAG_DONE);
        } else if (act == GID_SAVE) {
            char remote[AC_MAX_URL];
            get_field(g_gname, gname, sizeof(gname));
            get_field(g_gemail, gemail, sizeof(gemail));
            if (have_repo) {
                get_field(g_remote, remote, sizeof(remote));
                if (remote[0]) {
                    char server[AC_MAX_URL], name[256];
                    if (!vcs_url_split(remote, server, sizeof(server), name, sizeof(name))) {
                        GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text,
                                          (ULONG)"Keine Repository-URL (https://server/name).", TAG_DONE);
                        continue;
                    }
                }
                get_field(g_user, rc.user, sizeof(rc.user));
                get_field(g_rname, rc.name, sizeof(rc.name));
                get_field(g_remail, rc.email, sizeof(rc.email));
                vcs_repo_config_set(amivcs_dir, "remote", remote);
                vcs_repo_config_set(amivcs_dir, "user", rc.user);
                vcs_repo_config_set(amivcs_dir, "name", rc.name);
                vcs_repo_config_set(amivcs_dir, "email", rc.email);
                if (forget) vcs_repo_config_set(amivcs_dir, "pass", NULL);
                if (longnames != longnames_before)   /* then commit .amivcsproject */
                    vcs_config_file_set(projfile, "longnames", longnames ? "1" : "0");
            }
            vcs_global_set("name", gname);
            vcs_global_set("email", gemail);
            running = 0;
        } else if (act == GID_CANCEL || act == PREFSGUI_ACT_CLOSE) {
            running = 0;
        }
    }

    ac_winpos_save(WINPOS_VAR, win);
    CloseWindow(win);
    FreeGadgets(glist);
    prefsgui_screen_close();
    return 0;

fail:
    FreeGadgets(glist);
    prefsgui_screen_close();
    return 1;
}
