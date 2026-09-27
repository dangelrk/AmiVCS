/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef GUI_H
#define GUI_H

/*
 * gui.h — amivcs-gui, Amiga/MUI frontend (docs/design/
 * amivcs-gui-plan.md). Single window, git-gui style: staged/unstaged
 * change lists, a commit box, and a full multi-branch history list
 * whose leftmost column is an ASCII-art rendering of libcloudvcsgui's
 * commit graph (see historylist.h) -- no custom BOOPSI/graphics.library
 * drawing needed, unlike the Qt frontend's QPainter delegate, because
 * the same VcsGraphLayout data collapses naturally into text at the
 * Amiga's narrow console-ish window width.
 *
 * v1 scope, same as amicloud-commander-mui: synchronous operations with
 * a busy pointer, cancel handled by polling MUIM_Application_NewInput
 * from inside the ui_progress bridge during clone/push/pull -- no
 * worker Task/process.
 */

#include <exec/types.h>
#include <intuition/classes.h>

#include "session.h"

/* Main window assembly + event loop. Opens repo_path as the repository
 * (no picker) -- repo_path NULL/"" falls back to the current directory,
 * same rule as the CLI/Qt frontend (repo.h) -- offering "Init repository
 * here" if it isn't one yet. Returns a process exit code. */
int gui_run(const AcConfig *cfg, const char *repo_path);

#endif /* GUI_H */
