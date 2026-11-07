/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * gui.c — see gui.h. Modelled closely on amicloud-commander-mui/src/
 * gui.c's overall shape (Application/Window/MUIM_Application_Input event
 * loop, the ask_string() small input window, synchronous ui_progress
 * polling for cancel) -- single window instead of dual-pane, and the
 * domain is AMIVCS's staged/unstaged/commit/history/branches instead of
 * file transfers.
 */
#include <libraries/mui.h>
#include <libraries/iffparse.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <clib/alib_protos.h>

#include "gui.h"
#include "historylist.h"
#include "mui_stubs.h"
#include "ui_hooks.h"
#include "localfs.h"
#include "http.h"
#include "vcsname.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
#define ID_QUIT          1
#define ID_REFRESH       2
#define ID_INIT_REPO     3
#define ID_STAGE_SEL     4
#define ID_STAGE_ALL     5
#define ID_UNSTAGE_SEL   6
#define ID_COMMIT        7
#define ID_NEW_BRANCH    9
#define ID_SWITCH_BRANCH 10
#define ID_MERGE_BRANCH  11
#define ID_CLONE         12
#define ID_PUSH          13
#define ID_PULL          14
#define ID_ABOUT         15
#define ID_UNSTAGED_ACTIVE 16
#define ID_STAGED_ACTIVE   17
#define ID_UNSTAGED_DCLICK 18
#define ID_STAGED_DCLICK   19

/* TextEditor.mcc is optional (AmiVCS needs only MUI 3.8): with it the
 * commit message is multi-line like git gui's, without it a String
 * gadget stands in. The few ids are the class's own, from
 * mui/TextEditor_mcc.h, so the MCC SDK isn't needed to build. */
#ifndef MUIC_TextEditor
#define MUIC_TextEditor               "TextEditor.mcc"
#define MUIA_TextEditor_Contents      (0xad000000UL + 0x02)
#define MUIA_TextEditor_FixedFont     (0xad000000UL + 0x0a)
#define MUIM_TextEditor_ClearText     (0xad000000UL + 0x24)
#define MUIM_TextEditor_ExportText    (0xad000000UL + 0x25)
#endif

#define ID_INPUT_OK      900
#define ID_INPUT_CANCEL  901
#define ID_CANCEL_XFER   902

static Object *g_app;
static Object *g_status_text;
static Object *g_main_status;
static Object *g_unstaged_list, *g_staged_list, *g_history_list;
static Object *g_commit_string;     /* TextEditor or String, see g_msg_is_editor */
static int     g_msg_is_editor;
static Object *g_stage_all_check;
static Object *g_preview_title, *g_preview_text;
static void set_preview(const char *title, char *text);

/* vbcc's C library has no strdup() */
static char *dupstr(const char *t)
{
    char *c = (char *)malloc(strlen(t) + 1);
    if (c) strcpy(c, t);
    return c;
}

static Object *g_input_window, *g_input_string, *g_input_ok, *g_input_cancel;
static Object *g_progress_window, *g_progress_text, *g_cancel_button;
/* Put to sleep during clone/push/pull instead of the whole application:
 * MUIA_Application_Sleep also froze the progress window and its
 * "Abbrechen" button (found running amicloud-commander in Amiberry). */
static Object *g_main_window;

static VcsGuiSession g_session;
static AcConfig      g_cfg;
static char          g_cwd[AC_MAX_PATH];

static HistoryRow *g_history_rows;
static int         g_history_n;

/* ------------------------------------------------------------------ */
/* Change (staged/unstaged) list display hook -- single column, "kind    */
/* path", same label convention as `amivcs status`/the Qt frontend.      */
/* ------------------------------------------------------------------ */
static char g_disp_change[300];

static __saveds LONG change_display_func(__reg("a0") struct Hook *hook,
                                         __reg("a2") char **array,
                                         __reg("a1") VcsChange *c)
{
    (void)hook;
    if (!c) { array[0] = "Change"; return 0; }
    {
        const char *label = c->kind == 'N' ? "new" : c->kind == 'M' ? "modified" : "deleted";
        char shown[256];
        vcs_text_to_local_buf(c->path, shown, sizeof(shown));   /* repository path is UTF-8 */
        snprintf(g_disp_change, sizeof(g_disp_change), "%-9s %s", label, shown);
    }
    array[0] = g_disp_change;
    return 0;
}
static struct Hook change_display_hook = { { NULL, NULL }, (APTR)change_display_func, NULL, NULL };

/* ------------------------------------------------------------------ */
/* Small modal-ish string input window -- copied verbatim (generic, not  */
/* AMIVCS-specific) from amicloud-commander-mui/src/gui.c's              */
/* build_input_dialog()/ask_string(), the WbMan.c string-requester       */
/* pattern.                                                              */
/* ------------------------------------------------------------------ */
static void build_input_dialog(void)
{
    g_input_string = StringObject, StringFrame, MUIA_String_MaxLen, 512, End;
    g_input_ok     = SimpleButton("_OK");
    g_input_cancel = SimpleButton("_Cancel");

    g_input_window = WindowObject,
        MUIA_Window_ID,    MAKE_ID('I','N','P','T'),
        MUIA_Window_Title, "Input",
        MUIA_Window_Menu,  MUIV_Window_Menu_NoMenu,
        WindowContents, VGroup,
            Child, g_input_string,
            Child, HGroup,
                Child, g_input_ok,
                Child, g_input_cancel,
            End,
        End,
    End;

    DoMethod(g_app, OM_ADDMEMBER, g_input_window);
    DoMethod(g_input_window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
            g_app, 2, MUIM_Application_ReturnID, ID_INPUT_CANCEL);
    DoMethod(g_input_ok, MUIM_Notify, MUIA_Pressed, FALSE,
            g_app, 2, MUIM_Application_ReturnID, ID_INPUT_OK);
    DoMethod(g_input_cancel, MUIM_Notify, MUIA_Pressed, FALSE,
            g_app, 2, MUIM_Application_ReturnID, ID_INPUT_CANCEL);
    DoMethod(g_input_string, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
            g_app, 2, MUIM_Application_ReturnID, ID_INPUT_OK);
}

static int ask_string(const char *title, const char *initial, char *out, size_t outsz)
{
    ULONG sigs = 0;
    LONG  id;

    set(g_input_window, MUIA_Window_Title, title);
    set(g_input_string, MUIA_String_Contents, initial ? initial : "");
    set(g_input_window, MUIA_Window_Open, TRUE);
    set(g_input_window, MUIA_Window_ActiveObject, g_input_string);

    for (;;) {
        id = DoMethod(g_app, MUIM_Application_NewInput, &sigs);
        if (id == ID_INPUT_OK) {
            STRPTR text = NULL;
            get(g_input_string, MUIA_String_Contents, &text);
            if (text) { strncpy(out, (char *)text, outsz - 1); out[outsz - 1] = '\0'; }
            else out[0] = '\0';
            set(g_input_window, MUIA_Window_Open, FALSE);
            return 1;
        }
        if (id == ID_INPUT_CANCEL) {
            set(g_input_window, MUIA_Window_Open, FALSE);
            return 0;
        }
        if (sigs) Wait(sigs);
    }
}

/* ------------------------------------------------------------------ */
/* Remote progress/cancel bridge -- see session.h's vcsgui_install_
 * progress_hook()/VcsGuiSession::progress_cb. Synchronous polling of
 * MUIM_Application_NewInput exactly like amicloud-commander-mui's
 * mui_progress(), no worker Task (v1 scope, same as that app). */
static int mui_remote_progress_cb(void *userdata, long done, long total, const char *label)
{
    char buf[200];
    ULONG sigs = 0;
    (void)userdata;

    if (total > 0) snprintf(buf, sizeof(buf), "%s (%ld%%)", label ? label : "", (done * 100) / total);
    else           snprintf(buf, sizeof(buf), "%s", label ? label : "...");
    if (g_progress_text) set(g_progress_text, MUIA_Text_Contents, buf);

    DoMethod(g_app, MUIM_Application_NewInput, &sigs);
    return g_session.cancel_requested ? 0 : 1;
}

static __saveds LONG cancel_hook_func(__reg("a0") struct Hook *hook,
                                      __reg("a2") Object *obj,
                                      __reg("a1") APTR msg)
{
    (void)hook; (void)obj; (void)msg;
    vcsgui_request_cancel(&g_session);
    return 0;
}
static struct Hook cancel_hook = { { NULL, NULL }, (APTR)cancel_hook_func, NULL, NULL };

static void begin_remote_op(void)
{
    g_session.progress_cb = mui_remote_progress_cb;
    g_session.progress_userdata = NULL;
    set(g_progress_text, MUIA_Text_Contents, "");
    set(g_progress_window, MUIA_Window_Open, TRUE);
    if (g_main_window) set(g_main_window, MUIA_Window_Sleep, TRUE);
}

static void end_remote_op(void)
{
    if (g_main_window) set(g_main_window, MUIA_Window_Sleep, FALSE);
    set(g_progress_window, MUIA_Window_Open, FALSE);
    g_session.progress_cb = NULL;
}

/* ------------------------------------------------------------------ */
static void refresh_lists(void)
{
    int i;

    set(g_unstaged_list, MUIA_List_Quiet, TRUE);
    DoMethod(g_unstaged_list, MUIM_List_Clear);
    for (i = 0; i < g_session.unstaged.n; i++) {
        DoMethod(g_unstaged_list, MUIM_List_InsertSingle, &g_session.unstaged.items[i], MUIV_List_Insert_Bottom);
    }
    set(g_unstaged_list, MUIA_List_Quiet, FALSE);
    DoMethod(g_unstaged_list, MUIM_List_Redraw, MUIV_List_Redraw_All);

    set(g_staged_list, MUIA_List_Quiet, TRUE);
    DoMethod(g_staged_list, MUIM_List_Clear);
    for (i = 0; i < g_session.staged.n; i++) {
        DoMethod(g_staged_list, MUIM_List_InsertSingle, &g_session.staged.items[i], MUIV_List_Insert_Bottom);
    }
    set(g_staged_list, MUIA_List_Quiet, FALSE);
    DoMethod(g_staged_list, MUIM_List_Redraw, MUIV_List_Redraw_All);

    historylist_free(g_history_rows);
    g_history_rows = NULL;
    g_history_n = 0;
    historylist_build(&g_session, &g_history_rows, &g_history_n);

    set(g_history_list, MUIA_List_Quiet, TRUE);
    DoMethod(g_history_list, MUIM_List_Clear);
    for (i = 0; i < g_history_n; i++) {
        DoMethod(g_history_list, MUIM_List_InsertSingle, &g_history_rows[i], MUIV_List_Insert_Bottom);
    }
    set(g_history_list, MUIA_List_Quiet, FALSE);
    DoMethod(g_history_list, MUIM_List_Redraw, MUIV_List_Redraw_All);
}

static void refresh_ui(void)
{
    char buf[300];

    refresh_lists();
    set_preview("", NULL);

    if (g_session.is_open) {
        const char *branch = g_session.head.detached ? "(detached)" : g_session.head.branch;
        char shown[128];
        vcs_text_to_local_buf(branch, shown, sizeof(shown));
        snprintf(buf, sizeof(buf), "Current branch: \33b%s\33n      Repository: %s", shown, g_session.root);
    } else {
        strcpy(buf, "(not a repository -- Project/Init Repository Here)");
    }
    set(g_status_text, MUIA_Text_Contents, buf);
}

/* ------------------------------------------------------------------ */
/* File preview (git gui's right-hand pane, without the diff for now):   */
/* the selected file's current content from the working directory.       */
/* ------------------------------------------------------------------ */
#define PREVIEW_MAX 32768

/* One list row per source line -- Floattext would re-wrap code like
 * prose. Tabs become spaces (every 8 columns), very long lines are cut.
 * Takes ownership of `text`. */
static void set_preview(const char *title, char *text)
{
    char row[256];
    char *p = text;

    set(g_preview_title, MUIA_Text_Contents, title);
    set(g_preview_text, MUIA_List_Quiet, TRUE);
    DoMethod(g_preview_text, MUIM_List_Clear);
    while (p && *p) {
        int col = 0;
        while (*p && *p != '\n') {
            if (*p == '\t') {
                do { if (col < (int)sizeof(row) - 1) row[col] = ' '; col++; } while (col % 8);
            } else if (*p != '\r') {
                if (col < (int)sizeof(row) - 1) row[col] = *p;
                col++;
            }
            p++;
        }
        row[col < (int)sizeof(row) - 1 ? col : (int)sizeof(row) - 1] = '\0';
        DoMethod(g_preview_text, MUIM_List_InsertSingle, row, MUIV_List_Insert_Bottom);
        if (*p == '\n') p++;
    }
    set(g_preview_text, MUIA_List_Quiet, FALSE);
    free(text);
}

static void show_preview(const VcsChange *c)
{
    char shown[256], path[1024], title[320];
    const char *kind;
    BPTR fh;
    char *buf;
    LONG n, i;

    if (!c) { set_preview("", NULL); return; }
    kind = c->kind == 'N' ? "new" : c->kind == 'M' ? "modified" : "deleted";
    vcs_text_to_local_buf(c->path, shown, sizeof(shown));
    snprintf(title, sizeof(title), "\33b%s\33n  (%s)", shown, kind);
    if (c->kind == 'D') {
        set_preview(title, dupstr("(file deleted in the working directory)"));
        return;
    }
    strncpy(path, g_session.root, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    AddPart((STRPTR)path, (STRPTR)shown, sizeof(path));

    buf = (char *)malloc(PREVIEW_MAX + 64);
    if (!buf) { set_preview(title, NULL); return; }
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        strcpy(buf, "(cannot read the file)");
        set_preview(title, buf);
        return;
    }
    n = Read(fh, buf, PREVIEW_MAX);
    Close(fh);
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\0') {   /* binary: say so instead of showing noise */
            strcpy(buf, "(binary file)");
            set_preview(title, buf);
            return;
        }
        if (buf[i] == '\33') buf[i] = ' '; /* the list would read ESC as a style code */
    }
    buf[n] = '\0';
    if (n == PREVIEW_MAX) strcat(buf, "\n\n... (only the first 32 KB shown)");
    set_preview(title, buf);
}

static void preview_active(Object *list_obj, const VcsChangeList *changes)
{
    LONG active = -1;
    get(list_obj, MUIA_List_Active, &active);
    show_preview(active >= 0 && active < changes->n ? &changes->items[active] : NULL);
}

/* Double-click moves one file across, like clicking its icon in git gui. */
static void move_active(Object *list_obj, const VcsChangeList *changes, int stage)
{
    LONG active = -1;
    char path[1024];
    get(list_obj, MUIA_List_Active, &active);
    if (active < 0 || active >= changes->n) return;
    strncpy(path, changes->items[active].path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    if (stage) vcsgui_add(&g_session, path);
    else       vcsgui_remove(&g_session, path);
    refresh_ui();
}

/* ------------------------------------------------------------------ */
static void do_refresh(void) { refresh_ui(); }

static void open_or_offer_init(const char *cwd)
{
    if (vcsgui_open(&g_session, cwd)) { refresh_ui(); return; }

    if (MUI_Request(g_app, NULL, 0, "Not a repository", "_Yes|_No",
                    "This directory is not an AMIVCS repository yet.\nInitialize one here?") == 1) {
        if (!vcsgui_init_repo(&g_session, cwd)) {
            MUI_Request(g_app, NULL, 0, "Init failed", "_OK", "Could not initialize a repository here.");
        }
    }
    refresh_ui();
}

static void do_init_repo_here(void)
{
    if (g_session.is_open) {
        MUI_Request(g_app, NULL, 0, "Init", "_OK", "Already an open repository.");
        return;
    }
    if (!vcsgui_init_repo(&g_session, g_cwd)) {
        MUI_Request(g_app, NULL, 0, "Init failed", "_OK", "Could not initialize a repository here.");
    }
    refresh_ui();
}

/* Collects selected paths from a change list (list, matching session
 * list) into a small fixed array BEFORE calling into vcsgui_add()/
 * vcsgui_remove() -- those trigger vcsgui_refresh(), which reallocates
 * VcsGuiSession::staged/unstaged (see session.c), so indices/pointers
 * from before the first mutating call are no longer valid for the
 * second one. Same reasoning as panel_mui_next_selected()'s per-call
 * cookie, applied up front here instead of interleaved. */
#define MAX_BATCH 64
static int collect_selected_paths(Object *list_obj, const VcsChangeList *changes, char paths[MAX_BATCH][1024])
{
    LONG cookie = MUIV_List_NextSelected_Start;
    int indices[MAX_BATCH];
    int n = 0, npaths = 0, i;

    while (n < MAX_BATCH) {
        DoMethod(list_obj, MUIM_List_NextSelected, &cookie);
        if (cookie == MUIV_List_NextSelected_End) break;
        indices[n++] = (int)cookie;
    }
    if (n == 0) {
        LONG active = -1;
        get(list_obj, MUIA_List_Active, &active);
        if (active >= 0 && active < changes->n) indices[n++] = (int)active;
    }
    for (i = 0; i < n; i++) {
        int idx = indices[i];
        if (idx < 0 || idx >= changes->n) continue;
        strncpy(paths[npaths], changes->items[idx].path, sizeof(paths[npaths]) - 1);
        paths[npaths][sizeof(paths[npaths]) - 1] = '\0';
        npaths++;
    }
    return npaths;
}

static void do_stage_selected(void)
{
    char paths[MAX_BATCH][1024];
    int n = collect_selected_paths(g_unstaged_list, &g_session.unstaged, paths);
    int i;
    for (i = 0; i < n; i++) vcsgui_add(&g_session, paths[i]);
    refresh_ui();
}

static void do_unstage_selected(void)
{
    char paths[MAX_BATCH][1024];
    int n = collect_selected_paths(g_staged_list, &g_session.staged, paths);
    int i;
    for (i = 0; i < n; i++) vcsgui_remove(&g_session, paths[i]);
    refresh_ui();
}

static void do_stage_all(void)
{
    vcsgui_add_all(&g_session);
    refresh_ui();
}

/* The commit message as one malloc'd local-charset string, trailing
 * blank lines and spaces removed; NULL if it is empty. */
static char *get_commit_message(void)
{
    char *copy = NULL;
    size_t len;

    if (g_msg_is_editor) {
        STRPTR text = (STRPTR)DoMethod(g_commit_string, MUIM_TextEditor_ExportText);
        if (text) { copy = dupstr((char *)text); FreeVec(text); }
    } else {
        STRPTR text = NULL;
        get(g_commit_string, MUIA_String_Contents, &text);
        if (text) copy = dupstr((char *)text);
    }
    if (!copy) return NULL;
    len = strlen(copy);
    while (len > 0 && (copy[len - 1] == '\n' || copy[len - 1] == ' ' || copy[len - 1] == '\t'))
        copy[--len] = '\0';
    if (len == 0) { free(copy); return NULL; }
    return copy;
}

static void clear_commit_message(void)
{
    if (g_msg_is_editor) DoMethod(g_commit_string, MUIM_TextEditor_ClearText);
    else                 set(g_commit_string, MUIA_String_Contents, "");
}

static void do_commit(void)
{
    VcsOid oid;
    ULONG stage_all = FALSE;
    char *text;

    get(g_stage_all_check, MUIA_Selected, &stage_all);
    if (g_session.staged.n == 0 && !(stage_all && g_session.unstaged.n > 0)) {
        MUI_Request(g_app, g_main_window, 0, "Commit", "_OK",
                    "Nothing to commit.\n\n"
                    "Stage files first (double-click them in \"Unstaged Changes\",\n"
                    "or \"Stage selected\"/\"Stage all\"), or tick\n"
                    "\"Stage all changed files before commit\".");
        return;
    }
    text = get_commit_message();
    if (!text) {
        MUI_Request(g_app, g_main_window, 0, "Commit", "_OK",
                    "Please type a commit message into the\n"
                    "\"Commit message\" field first.");
        set(g_main_window, MUIA_Window_ActiveObject, g_commit_string);
        return;
    }
    {
        char *msg = vcs_text_from_local(text);   /* repository text is UTF-8 */
        int ok = msg && vcsgui_commit(&g_session, msg, stage_all ? 1 : 0, &oid);
        free(msg);
        free(text);
        if (!ok) {
            MUI_Request(g_app, g_main_window, 0, "Commit", "_OK", (char *)vcsgui_last_message());
            return;
        }
    }
    clear_commit_message();
    set(g_stage_all_check, MUIA_Selected, FALSE);
    set(g_main_status, MUIA_Text_Contents, "Committed.");
    refresh_ui();
}

static void show_branch_list_hint(void)
{
    char msg[1024];
    int i, used;

    strcpy(msg, "Available branches:\n");
    used = strlen(msg);
    for (i = 0; i < g_session.branches.n && used < (int)sizeof(msg) - 64; i++) {
        const char *full = g_session.branches.items[i].name;
        const char *short_name = strncmp(full, "refs/heads/", 11) == 0 ? full + 11 : full;
        char shown[128];
        int n;
        vcs_text_to_local_buf(short_name, shown, sizeof(shown));
        n = snprintf(msg + used, sizeof(msg) - (size_t)used, "  %s\n", shown);
        if (n > 0) used += n;
    }
    MUI_Request(g_app, NULL, 0, "Branches", "_OK", msg);
}

static void do_new_branch(void)
{
    char name[128];
    if (!ask_string("New branch name", "", name, sizeof(name))) return;
    if (!name[0]) return;
    {   /* branch names are UTF-8 in the repository */
        char *u = g_session.is_open ? vcs_repo_branch_name(&g_session.repo, name) : vcs_text_from_local(name);
        if (u) { vcs_utf8_copy(name, sizeof(name), u); free(u); }
    }
    if (!vcsgui_branch_create(&g_session, name)) {
        MUI_Request(g_app, NULL, 0, "New Branch", "_OK", (char *)vcsgui_last_message());
    }
    refresh_ui();
}

static void do_switch_branch(void)
{
    char name[128];
    if (g_session.branches.n == 0) {
        MUI_Request(g_app, NULL, 0, "Switch Branch", "_OK", "No branches yet -- commit first.");
        return;
    }
    show_branch_list_hint();
    if (!ask_string("Switch to branch", "", name, sizeof(name))) return;
    if (!name[0]) return;
    {   /* branch names are UTF-8 in the repository */
        char *u = g_session.is_open ? vcs_repo_branch_name(&g_session.repo, name) : vcs_text_from_local(name);
        if (u) { vcs_utf8_copy(name, sizeof(name), u); free(u); }
    }
    if (!vcsgui_switch(&g_session, name)) {
        MUI_Request(g_app, NULL, 0, "Switch Branch", "_OK", (char *)vcsgui_last_message());
    }
    refresh_ui();
}

static void do_merge_branch(void)
{
    char name[128];
    if (g_session.branches.n == 0) {
        MUI_Request(g_app, NULL, 0, "Merge Branch", "_OK", "No branches yet -- commit first.");
        return;
    }
    show_branch_list_hint();
    if (!ask_string("Merge which branch into the current one?", "", name, sizeof(name))) return;
    if (!name[0]) return;
    {   /* branch names are UTF-8 in the repository */
        char *u = g_session.is_open ? vcs_repo_branch_name(&g_session.repo, name) : vcs_text_from_local(name);
        if (u) { vcs_utf8_copy(name, sizeof(name), u); free(u); }
    }
    if (!vcsgui_merge(&g_session, name)) {
        MUI_Request(g_app, NULL, 0, "Merge", "_OK", (char *)vcsgui_last_message());
    } else {
        set(g_main_status, MUIA_Text_Contents, vcsgui_last_message());
    }
    refresh_ui();
}

/* Like git, everything except clone/push/pull works without any network:
 * bsdsocket.library/AmiSSL are only opened on the first remote operation
 * (main_mui.c no longer calls http_init() at startup). If that fails the
 * reason is shown and the window stays open for local work; the next
 * remote operation simply tries again (e.g. after starting the TCP/IP
 * stack). http_cleanup() in main_mui.c is safe either way. */
static int g_net_ready = 0;

static int ensure_network(const char *title)
{
    if (g_net_ready) return 1;
    if (!http_init()) {
        const char *why = http_last_error();
        MUI_Request(g_app, NULL, 0, (char *)title, "_OK",
                    "Network initialisation failed%s%s\n\n"
                    "Clone, Push and Pull need a running TCP/IP stack\n"
                    "(bsdsocket.library) and AmiSSL 5. Local work is\n"
                    "not affected.",
                    (char *)(*why ? ":\n" : "."), (char *)why);
        return 0;
    }
    g_net_ready = 1;
    return 1;
}

static int ensure_remote_configured(void)
{
    char server[AC_MAX_URL], user[64], pass[128];

    if (g_cfg.server[0] && g_cfg.username[0] && g_cfg.password[0]) return 1;

    if (!ask_string("Remote server URL", g_cfg.server, server, sizeof(server)) || !server[0]) return 0;
    if (!ask_string("Remote username", g_cfg.username, user, sizeof(user)) || !user[0]) return 0;
    if (!ask_string("Remote password", "", pass, sizeof(pass)) || !pass[0]) return 0;

    strncpy(g_cfg.server, server, sizeof(g_cfg.server) - 1);
    strncpy(g_cfg.username, user, sizeof(g_cfg.username) - 1);
    strncpy(g_cfg.password, pass, sizeof(g_cfg.password) - 1);
    g_cfg.verify_ssl = 1;
    if (!g_cfg.basepath[0]) { g_cfg.basepath[0] = '/'; g_cfg.basepath[1] = '\0'; }
    return 1;
}

static void do_clone(void)
{
    char repo_name[128], local_dir[512];
    int ok;

    if (!ensure_network("Clone")) return;
    if (!ensure_remote_configured()) return;
    if (!ask_string("Clone: repository name on server", "", repo_name, sizeof(repo_name)) || !repo_name[0]) return;
    if (!ask_string("Clone: local directory", "", local_dir, sizeof(local_dir)) || !local_dir[0]) return;

    begin_remote_op();
    ok = vcsgui_clone(&g_session, &g_cfg, repo_name, local_dir);
    end_remote_op();

    if (!ok) MUI_Request(g_app, NULL, 0, "Clone", "_OK", (char *)vcsgui_last_message());
    else set(g_main_status, MUIA_Text_Contents, vcsgui_last_message());
    refresh_ui();
}

static void do_push(void)
{
    int ok;
    if (!g_session.is_open) return;
    if (!ensure_network("Push")) return;
    if (!ensure_remote_configured()) return;

    begin_remote_op();
    ok = vcsgui_push(&g_session, &g_cfg, NULL);
    end_remote_op();

    if (!ok) MUI_Request(g_app, NULL, 0, "Push", "_OK", (char *)vcsgui_last_message());
    else set(g_main_status, MUIA_Text_Contents, vcsgui_last_message());
    refresh_ui();
}

static void do_pull(void)
{
    int ok;
    if (!g_session.is_open) return;
    if (!ensure_network("Pull")) return;
    if (!ensure_remote_configured()) return;

    begin_remote_op();
    ok = vcsgui_pull(&g_session, &g_cfg, NULL, NULL);
    end_remote_op();

    if (!ok) MUI_Request(g_app, NULL, 0, "Pull", "_OK", (char *)vcsgui_last_message());
    else set(g_main_status, MUIA_Text_Contents, vcsgui_last_message());
    refresh_ui();
}

static void do_about(void)
{
    char msg[256];
    snprintf(msg, sizeof(msg),
            "amivcs-gui v%s\nMinimal git-gui-style AmiVCS frontend\n\n"
            "(C) 2026 Ronny Krueger\n"
            "GNU General Public License v3 or later", AC_VERSION);
    MUI_Request(g_app, NULL, 0, "About amivcs-gui", "_OK", msg);
}

/* ------------------------------------------------------------------ */
static struct NewMenu MenuData[] = {
    { NM_TITLE, "Project", 0, 0, 0, (APTR)0 },
    { NM_ITEM,  "About...", "?", 0, 0, (APTR)ID_ABOUT },
    { NM_ITEM,  NM_BARLABEL, 0, 0, 0, (APTR)0 },
    { NM_ITEM,  "Quit", "Q", 0, 0, (APTR)ID_QUIT },

    { NM_TITLE, "Repository", 0, 0, 0, (APTR)0 },
    { NM_ITEM,  "Init Repository Here", 0, 0, 0, (APTR)ID_INIT_REPO },
    { NM_ITEM,  "Refresh", "R", 0, 0, (APTR)ID_REFRESH },
    { NM_ITEM,  NM_BARLABEL, 0, 0, 0, (APTR)0 },
    { NM_ITEM,  "New Branch...", "B", 0, 0, (APTR)ID_NEW_BRANCH },
    { NM_ITEM,  "Switch Branch...", "S", 0, 0, (APTR)ID_SWITCH_BRANCH },
    { NM_ITEM,  "Merge Branch...", "M", 0, 0, (APTR)ID_MERGE_BRANCH },

    { NM_TITLE, "Remote", 0, 0, 0, (APTR)0 },
    { NM_ITEM,  "Clone...", 0, 0, 0, (APTR)ID_CLONE },
    { NM_ITEM,  "Push", "P", 0, 0, (APTR)ID_PUSH },
    { NM_ITEM,  "Pull", "L", 0, 0, (APTR)ID_PULL },

    { NM_END, NULL, 0, 0, 0, (APTR)0 },
};

int gui_run(const AcConfig *cfg, const char *repo_path)
{
    static const char *pageTitles[] = { "Changes", "History", NULL };
    Object *window;
    Object *bt_stage, *bt_stage_all, *bt_unstage, *bt_commit;
    Object *bt_refresh, *bt_new_branch, *bt_switch, *bt_merge, *bt_clone, *bt_push, *bt_pull, *bt_quit;
    Object *lv_unstaged, *lv_staged;
    ULONG   sigs = 0;
    BOOL    running = TRUE;

    vcsgui_session_init(&g_session);
    g_cfg = *cfg;
    {
        char *author = cfg->username[0] ? vcs_text_from_local(cfg->username) : NULL;
        vcsgui_set_author(&g_session, author);
        free(author);
    }
    if (repo_path && repo_path[0]) {
        strncpy(g_cwd, repo_path, sizeof(g_cwd) - 1);
        g_cwd[sizeof(g_cwd) - 1] = '\0';
    } else if (!localfs_getcwd(g_cwd, sizeof(g_cwd))) {
        g_cwd[0] = '\0';
    }

    vcsgui_install_hooks();

    g_status_text = TextObject, MUIA_Text_Contents, "", End;
    g_main_status = TextObject, MUIA_Text_Contents, "", End;

    g_unstaged_list = ListObject, InputListFrame, MUIA_List_DisplayHook, &change_display_hook, End;
    g_staged_list   = ListObject, InputListFrame, MUIA_List_DisplayHook, &change_display_hook, End;
    lv_unstaged = ListviewObject, MUIA_Listview_List, g_unstaged_list,
                      MUIA_Listview_MultiSelect, MUIV_Listview_MultiSelect_Shifted, End;
    lv_staged   = ListviewObject, MUIA_Listview_List, g_staged_list,
                      MUIA_Listview_MultiSelect, MUIV_Listview_MultiSelect_Shifted, End;

    bt_stage      = SimpleButton("_Stage selected");
    bt_stage_all  = SimpleButton("Stage _all");
    bt_unstage    = SimpleButton("_Unstage selected");

    g_preview_title = TextObject, MUIA_Text_Contents, "", End;
    g_preview_text  = ListObject, ReadListFrame,
                          MUIA_Font, MUIV_Font_Fixed,
                          MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                          MUIA_List_DestructHook,  MUIV_List_DestructHook_String,
                      End;

    /* multi-line like git gui if TextEditor.mcc is there */
    g_commit_string = MUI_NewObject(MUIC_TextEditor,
                          MUIA_Frame, MUIV_Frame_String,
                          MUIA_TextEditor_FixedFont, TRUE,
                          MUIA_CycleChain, 1,
                          TAG_DONE);
    g_msg_is_editor = g_commit_string != NULL;
    if (!g_commit_string)
        g_commit_string = StringObject, StringFrame, MUIA_String_MaxLen, 500, MUIA_CycleChain, 1, End;
    g_stage_all_check = CheckMark(FALSE);
    bt_commit         = SimpleButton("_Commit");

    g_history_list = ListObject,
                        InputListFrame,
                        MUIA_List_Title,       TRUE,
                        MUIA_List_Format,      ",BAR,,BAR,,BAR,,BAR,",
                        MUIA_List_DisplayHook, &history_display_hook,
                    End;

    bt_refresh    = SimpleButton("_Rescan");
    bt_new_branch = SimpleButton("New Branch...");
    bt_switch     = SimpleButton("Switch...");
    bt_merge      = SimpleButton("Merge...");
    bt_clone      = SimpleButton("Clone...");
    bt_push       = SimpleButton("_Push");
    bt_pull       = SimpleButton("Pu_ll");
    bt_quit       = NULL;

    g_progress_text   = TextObject, MUIA_Text_Contents, "", End;
    g_cancel_button   = SimpleButton("_Cancel");
    g_progress_window = WindowObject,
        MUIA_Window_ID,    MAKE_ID('P','R','O','G'),
        MUIA_Window_Title, "Working...",
        MUIA_Window_CloseGadget, FALSE,
        MUIA_Window_Menu, MUIV_Window_Menu_NoMenu,
        MUIA_Window_Width, MUIV_Window_Width_Screen(33),
        WindowContents, VGroup,
            Child, g_progress_text,
            Child, g_cancel_button,
        End,
    End;

    g_app = ApplicationObject,
        MUIA_Application_Title,       "amivcs-gui",
        MUIA_Application_Version,     "$VER: amivcs-gui " AC_VERSION " (" AC_DATE ")",
        MUIA_Application_Copyright,   "(C) 2026 Ronny Krueger",
        MUIA_Application_Author,      "Ronny Krueger",
        MUIA_Application_Description, "AmiVCS git-gui-style frontend",
        MUIA_Application_Base,        "AMIVCSGUI",

        SubWindow, window = WindowObject,
            MUIA_Window_ID,    MAKE_ID('M','A','I','N'),
            MUIA_Window_Title, "amivcs-gui",
            MUIA_Window_Width,  MUIV_Window_Width_Screen(70),
            MUIA_Window_Height, MUIV_Window_Height_Screen(80),
            MUIA_Window_Menustrip, MUI_MakeObject(MUIO_MenustripNM, MenuData, 0),
            WindowContents, VGroup,
                Child, g_status_text,
                Child, RegisterGroup(pageTitles),
                    MUIA_Register_Frame, TRUE,

                    /* ---- page 1: git gui's main window ---- */
                    Child, VGroup,
                        Child, HGroup,
                            MUIA_VertWeight, 70,
                            Child, VGroup,
                                MUIA_HorizWeight, 35,
                                Child, TextObject, MUIA_Text_Contents, "\33bUnstaged Changes", End,
                                Child, lv_unstaged,
                                Child, HGroup,
                                    Child, bt_stage,
                                    Child, bt_stage_all,
                                End,
                                Child, BalanceObject, End,
                                Child, TextObject, MUIA_Text_Contents, "\33bStaged Changes (Will Commit)", End,
                                Child, lv_staged,
                                Child, bt_unstage,
                            End,
                            Child, BalanceObject, End,
                            Child, VGroup,
                                MUIA_HorizWeight, 65,
                                Child, g_preview_title,
                                Child, ListviewObject,
                                    MUIA_Listview_Input, FALSE,
                                    MUIA_Listview_List, g_preview_text,
                                End,
                            End,
                        End,
                        Child, BalanceObject, End,
                        Child, HGroup,
                            MUIA_VertWeight, 30,
                            Child, VGroup,
                                MUIA_HorizWeight, 0,
                                Child, bt_refresh,
                                Child, bt_commit,
                                Child, bt_push,
                                Child, bt_pull,
                                Child, VSpace(0),
                            End,
                            Child, VGroup,
                                Child, TextObject, MUIA_Text_Contents, "\33bCommit message:", End,
                                Child, g_commit_string,
                                Child, HGroup,
                                    Child, g_stage_all_check,
                                    Child, LLabel1("Stage all changed files before commit (-a)"),
                                    Child, HSpace(0),
                                End,
                            End,
                        End,
                    End,

                    /* ---- page 2: history and branches ---- */
                    Child, VGroup,
                        Child, ListviewObject, MUIA_Listview_List, g_history_list, End,
                        Child, HGroup,
                            Child, bt_new_branch,
                            Child, bt_switch,
                            Child, bt_merge,
                            Child, bt_clone,
                        End,
                    End,
                End,
                Child, g_main_status,
            End,
        End,
    End;

    if (!g_app) return 20;
    g_main_window = window;

    DoMethod(g_app, OM_ADDMEMBER, g_progress_window);
    build_input_dialog();

    /* every window reopens where it was left, also after a reboot */
    mui_remember_window(window);
    mui_remember_window(g_input_window);
    mui_remember_window(g_progress_window);

    DoMethod(window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
            g_app, 2, MUIM_Application_ReturnID, ID_QUIT);

    (void)bt_quit;
    DoMethod(bt_refresh, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_REFRESH);
    DoMethod(bt_stage, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_STAGE_SEL);
    DoMethod(bt_stage_all, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_STAGE_ALL);
    DoMethod(bt_unstage, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_UNSTAGE_SEL);
    DoMethod(bt_commit, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_COMMIT);
    DoMethod(g_unstaged_list, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, g_app, 2, MUIM_Application_ReturnID, ID_UNSTAGED_ACTIVE);
    DoMethod(g_staged_list, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, g_app, 2, MUIM_Application_ReturnID, ID_STAGED_ACTIVE);
    DoMethod(lv_unstaged, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, g_app, 2, MUIM_Application_ReturnID, ID_UNSTAGED_DCLICK);
    DoMethod(lv_staged, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, g_app, 2, MUIM_Application_ReturnID, ID_STAGED_DCLICK);
    DoMethod(bt_new_branch, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_NEW_BRANCH);
    DoMethod(bt_switch, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_SWITCH_BRANCH);
    DoMethod(bt_merge, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_MERGE_BRANCH);
    DoMethod(bt_clone, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_CLONE);
    DoMethod(bt_push, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_PUSH);
    DoMethod(bt_pull, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_Application_ReturnID, ID_PULL);
    DoMethod(g_cancel_button, MUIM_Notify, MUIA_Pressed, FALSE, g_app, 2, MUIM_CallHook, &cancel_hook);

    open_or_offer_init(g_cwd);
    set(window, MUIA_Window_Open, TRUE);

    while (running) {
        LONG id = DoMethod(g_app, MUIM_Application_Input, &sigs);
        switch (id) {
        case ID_QUIT:           running = FALSE; break;
        case ID_REFRESH:        do_refresh(); break;
        case ID_INIT_REPO:      do_init_repo_here(); break;
        case ID_STAGE_SEL:      do_stage_selected(); break;
        case ID_STAGE_ALL:      do_stage_all(); break;
        case ID_UNSTAGE_SEL:    do_unstage_selected(); break;
        case ID_COMMIT:         do_commit(); break;
        case ID_UNSTAGED_ACTIVE: preview_active(g_unstaged_list, &g_session.unstaged); break;
        case ID_STAGED_ACTIVE:   preview_active(g_staged_list, &g_session.staged); break;
        case ID_UNSTAGED_DCLICK: move_active(g_unstaged_list, &g_session.unstaged, 1); break;
        case ID_STAGED_DCLICK:   move_active(g_staged_list, &g_session.staged, 0); break;
        case ID_NEW_BRANCH:     do_new_branch(); break;
        case ID_SWITCH_BRANCH:  do_switch_branch(); break;
        case ID_MERGE_BRANCH:   do_merge_branch(); break;
        case ID_CLONE:          do_clone(); break;
        case ID_PUSH:           do_push(); break;
        case ID_PULL:           do_pull(); break;
        case ID_ABOUT:          do_about(); break;
        default: break;
        }
        /* A Ctrl-C (only possible when started from a Shell) cancels the
         * transfer it hit, see ui_hooks.h -- never every later one. */
        ui_break_clear();
        if (running && sigs) Wait(sigs);
    }

    set(window, MUIA_Window_Open, FALSE);
    historylist_free(g_history_rows);

    vcsgui_close(&g_session);
    MUI_DisposeObject(g_app);
    return 0;
}
