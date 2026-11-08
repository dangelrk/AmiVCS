/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_VCSCONFIG_H
#define CLOUDVCS_VCSCONFIG_H

/*
 * vcsconfig.h -- where a repository's remote, its credentials and the
 * commit identity come from (docs/design/remotes-plan.md):
 *
 *   .amivcs/config      per repository, not versioned:
 *                         remote=https://host/path/<repository>
 *                         user=<login>        pass=<password, if remembered>
 *                         name=<author>       email=<address>
 *   global defaults     ENV:AmiVCS/config (ENVARC: as the durable copy)
 *                       on the Amiga, ~/.amivcsconfig elsewhere:
 *                         name=  email=  and the program options
 *                         charset= timeout= verify_ssl= debug=
 *
 * Both are plain "key=value" lines in the local codeset. The identity is
 * the repository's own, else the global one -- like git's user.name and
 * user.email. A remote URL's last path element is the repository's name
 * on the server, everything before it the server base (api.php lives
 * there).
 */

#include <stddef.h>
#include "amicloud.h"

typedef struct {
    char remote[AC_MAX_URL];
    char name[128];
    char email[128];
    char user[64];
    char pass[128];
} VcsRepoConfig;

/* ---- plain key=value files ---- */

/* 1 and *out filled if `key` is set (non-empty) in `path`. */
int vcs_config_file_get(const char *path, const char *key, char *out, size_t sz);
/* Sets `key` (keeps every other line, in order); val NULL or "" removes
 * it. Written through a temporary file. Returns 1 on success. */
int vcs_config_file_set(const char *path, const char *key, const char *val);

/* ---- global defaults ---- */

/* The file the global values are read from (Amiga: ENV:, else ENVARC:). */
void vcs_global_config_path(char *out, size_t sz);
int  vcs_global_get(const char *key, char *out, size_t sz);
/* Amiga: writes ENV: and ENVARC: (directories made as needed). */
int  vcs_global_set(const char *key, const char *val);
/* Fills the program options (charset, timeout, verify_ssl, debug) of
 * *cfg from the global file; leaves everything else alone. */
void vcs_global_load_options(AcConfig *cfg);

/* ---- per repository ---- */

void vcs_repo_config_path(const char *amivcs_dir, char *out, size_t sz);
/* Zeroes *rc, then fills what .amivcs/config has. Returns 1 if the file
 * exists. */
int  vcs_repo_config_load(const char *amivcs_dir, VcsRepoConfig *rc);
int  vcs_repo_config_set(const char *amivcs_dir, const char *key, const char *val);

/* The commit author, "Name <email>" or just "Name": the repository's
 * name/email first, the global ones otherwise (amivcs_dir may be NULL).
 * Returns 0 (and an empty *out) if no name is set anywhere. */
int  vcs_identity(const char *amivcs_dir, char *out, size_t sz);

/* ---- remote URL ---- */

/* "https://host/a/b/repo" -> server "https://host/a/b", repo "repo".
 * A trailing '/' is ignored. Returns 0 for anything that is not
 * scheme://host/<name>. */
int  vcs_url_split(const char *url, char *server, size_t ssz, char *repo, size_t rsz);

/* Fills cfg->server, cfg->username, cfg->password for the repository's
 * remote: `url` if given (not remembered here), else remote= from
 * .amivcs/config; user=/pass= from .amivcs/config. *repo_name gets the
 * repository's name on the server. Returns 0 (with a message) if there
 * is no usable remote URL. An empty password is left for the caller to
 * ask for. */
int  vcs_remote_resolve(const char *amivcs_dir, const char *url, AcConfig *cfg,
                        char *repo_name, size_t rsz);

#endif /* CLOUDVCS_VCSCONFIG_H */
