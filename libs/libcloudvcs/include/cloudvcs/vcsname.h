/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_VCSNAME_H
#define CLOUDVCS_VCSNAME_H

/*
 * vcsname.h -- names and texts between the repository and the local
 * system.
 *
 * Inside a repository (tree entry names, index, commit author and
 * message, branch names, every relative path the libcloudvcs API takes
 * or returns) text is UTF-8, NFC -- the same on the Amiga and the PC, so
 * a repository moves between them without "GrÃ¶ÃŸe.txt". The local side
 * is the file system's codeset (charset.h: codesets.library on the
 * Amiga, ISO-8859-1 without it; UTF-8 on the PC). File contents are
 * never converted.
 *
 * Older names: builds up to 0.5 stored what the system handed them. A
 * name that is not valid UTF-8 keeps its bytes in the repository (it is
 * never rewritten) and on disk:
 *   Amiga  its bytes are the local name already (any codeset);
 *   PC     it is read as ISO-8859-1 (also what the server's web view
 *          does) and written as UTF-8; a local name that is not valid
 *          UTF-8 is taken as it is.
 *
 * Name table. Some repository names have a local form that does not
 * convert back to them by the plain rule: a character the codeset lacks
 * (written as charset.h's CS_REPLACEMENT), an older name, a decomposed
 * name. Each operation starts a fresh table (vcs_names_reset()) and
 * teaches it the trees it works with (vcs_names_learn()), so the
 * working-tree scan maps such a local name back to its repository name
 * instead of seeing a delete plus an add. Between a merge with conflicts
 * and the next commit the table is kept in .amivcs/localnames
 * (vcs_names_save()/vcs_names_load()). Trees in which two repository
 * names would get the same local name are refused before anything is
 * written (vcs_names_check()).
 *
 * Not reentrant (static buffers keep the Amiga stack small). Call
 * cs_open() once at program start.
 */

#include <stddef.h>

/* --- name table (one operation) --- */
void vcs_names_reset(void);
/* teaches one repository path (and its parent directories) */
void vcs_names_learn(const char *repo_path);
/* .amivcs/localnames: load adds its pairs, save writes the table's
 * pairs (removes the file if there are none), forget removes it */
void vcs_names_load(const char *amivcs_dir);
void vcs_names_save(const char *amivcs_dir);
void vcs_names_forget(const char *amivcs_dir);
/* after a commit: drops the file's pairs whose repository path the new
 * HEAD now carries (settled() returns 1 for those), keeps the rest (a
 * file of a merge not committed yet) */
void vcs_names_prune(const char *amivcs_dir, int (*settled)(const char *repo_path, void *ctx), void *ctx);

/* Collision check over n repository paths (files; parent directories
 * are checked too): 1 if all local forms differ, else 0 with both
 * repository names in a and b. */
int vcs_names_check(const char *const *paths, int n, char *a, char *b, size_t sz);

/* --- paths --- */
/* Repository path ("dir/file") -> local relative path. Returns 1 if the
 * local name converts back to it by the plain rule, 0 if not (a table
 * pair is then needed), -1 if out does not hold it (out is ""). */
int  vcs_path_to_local(const char *repo_path, char *out, size_t outsz);

/* Local relative path -> repository path: the table first (also for
 * each parent directory), else the plain rule per component. Returns 1,
 * or 0 if out does not hold it (out is ""). */
int  vcs_path_from_local(const char *local_path, char *out, size_t outsz);

/* root (local) + repository relative path -> absolute local path.
 * Returns 0 (out "") if it does not fit. */
int  vcs_local_join(char *out, size_t outsz, const char *root, const char *repo_rel);

/* --- texts (author, message, branch names) --- */
/* text from a repository object as valid UTF-8 (older text converted).
 * malloc'd, NULL only when out of memory */
char *vcs_text_utf8(const char *s);
/* repository text -> local codeset (console, Amiga GUI); '?' is kept.
 * malloc'd, NULL only when out of memory */
char *vcs_text_to_local(const char *s);
/* same into a buffer (display hooks), truncated to whole characters */
void  vcs_text_to_local_buf(const char *s, char *out, size_t outsz);
/* local text (command line, Amiga GUI, config) -> UTF-8 NFC.
 * malloc'd, NULL only when out of memory */
char *vcs_text_from_local(const char *s);

/* copies UTF-8 src into dst, cut at a character boundary */
void  vcs_utf8_copy(char *dst, size_t dstsz, const char *src);
/* 1 if s is valid UTF-8 (pure ASCII included) */
int   vcs_utf8_valid(const char *s);

#endif /* CLOUDVCS_VCSNAME_H */
