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
/* same for a message argument: a static buffer (four in rotation, 512
 * bytes each, truncated), so up to four may appear in one ui_notify() */
const char *vcs_shown(const char *s);
/* local text (command line, Amiga GUI, config) -> UTF-8 NFC.
 * malloc'd, NULL only when out of memory */
char *vcs_text_from_local(const char *s);

/* copies UTF-8 src into dst, cut at a character boundary */
void  vcs_utf8_copy(char *dst, size_t dstsz, const char *src);
/* 1 if s is valid UTF-8 (pure ASCII included) */
int   vcs_utf8_valid(const char *s);

/* Branch names (UTF-8, without "refs/heads/"): 1 if the server accepts
 * it, the same rule as server/includes/RefStore.php validRefName():
 * 1..100 bytes of valid UTF-8, no control characters, no space and none
 * of ~ ^ : ? * [ \ (git's rule), no "..", "//" or "@{", not starting
 * with '/', '-' or '.', not ending with '/', '.' or ".lock". Umlauts and
 * other letters are fine. */
#define VCS_BRANCH_MAX 100
int   vcs_branch_name_ok(const char *name);

/* --- name length ---
 * Classic FFS/OFS keep 30 characters per name and truncate a longer one
 * silently, when creating AND when opening -- two long names with the
 * same first 30 characters become one file. Nothing is written under a
 * name the working tree's file system would not keep. */

/* Longest name (bytes, local form) the file system under root keeps:
 * localfs_name_max(), probed once per root and remembered. */
int vcs_name_max(const char *root);

/* Bytes of one repository name component in its local form, -1 if it
 * cannot be converted. */
int vcs_local_name_len(const char *repo_name);

/* 1 if every component of all n repository paths fits name_max in its
 * local form; else 0 and the first path that does not (up to and
 * including the long component) in bad. */
int vcs_names_fit(const char *const *paths, int n, int name_max, char *bad, size_t sz);

/* Repository-relative name of the "theirs" copy a conflicted merge
 * writes next to repo_path: "<path>.theirs", or -- when that is longer
 * than name_max locally -- with the file name cut so that "<cut>.theirs"
 * fits (".theirs" stays, tools recognise the copy by it). Deterministic:
 * the same path and name_max always give the same name. Returns 0 if out
 * cannot hold it. */
int vcs_theirs_rel(const char *repo_path, int name_max, char *out, size_t sz);

#endif /* CLOUDVCS_VCSNAME_H */
