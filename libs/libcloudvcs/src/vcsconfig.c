/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * vcsconfig.c -- see vcsconfig.h.
 */
#include "vcsconfig.h"
#include "config.h"
#include "localfs.h"
#include "ui_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLATFORM_AMIGA
#include <dos/dos.h>
#include <proto/dos.h>
#endif

#define REPO_CONFIG_FILE "config"

/* ------------------------------------------------------------------ */
/* key=value files                                                       */
/* ------------------------------------------------------------------ */

/* Splits "key = value" (blanks around both allowed); 0 for comments and
 * lines without '='. */
static int split_line(char *line, char **key, char **val)
{
    char *eq, *end;

    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == ';' || *line == '\0') return 0;
    eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = '\0';
    end = eq;
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    *key = line;
    line = eq + 1;
    while (*line == ' ' || *line == '\t') line++;
    end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    *val = line;
    return 1;
}

int vcs_config_file_get(const char *path, const char *key, char *out, size_t sz)
{
    FILE *fp = fopen(path, "r");
    char line[1200];
    int found = 0;

    if (sz) out[0] = '\0';
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        char *k, *v;
        if (split_line(line, &k, &v) && strcmp(k, key) == 0 && v[0]) {
            strncpy(out, v, sz - 1);
            out[sz - 1] = '\0';
            found = 1;   /* the last one wins, like a later assignment */
        }
    }
    fclose(fp);
    return found;
}

/* Copies src over dst (truncating it). Returns 1 on success. */
static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    char buf[512];
    size_t n;
    int ok = 1;

    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok;
}

int vcs_config_file_set(const char *path, const char *key, const char *val)
{
    char tmp[AC_MAX_PATH + 8];
    FILE *in, *out;
    char line[1200], copy[1200];
    int written = 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    out = fopen(tmp, "w");
    if (!out) return 0;
    in = fopen(path, "r");
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char *k, *v;
            strcpy(copy, line);
            if (split_line(copy, &k, &v) && strcmp(k, key) == 0) {
                if (val && val[0] && !written) fprintf(out, "%s=%s\n", key, val);
                written = 1;
                continue;
            }
            fputs(line, out);
            if (line[0] && line[strlen(line) - 1] != '\n') fputc('\n', out);
        }
        fclose(in);
    }
    if (!written && val && val[0]) fprintf(out, "%s=%s\n", key, val);
    if (fclose(out) != 0) { remove(tmp); return 0; }
    remove(path);
    if (rename(tmp, path) != 0) {
        /* ENV: on OS 3.2 (ram-handler 47) shows ENVARC:'s file where the
         * RAM copy was just deleted, so the rename fails with "object
         * exists". Write the new content over it instead. */
        int ok = copy_file(tmp, path);
        remove(tmp);
        return ok;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* global defaults                                                       */
/* ------------------------------------------------------------------ */

void vcs_global_config_path(char *out, size_t sz)
{
#ifdef PLATFORM_AMIGA
    BPTR lock = Lock((STRPTR)"ENV:AmiVCS/config", ACCESS_READ);
    if (lock) { UnLock(lock); snprintf(out, sz, "ENV:AmiVCS/config"); }
    else      snprintf(out, sz, "ENVARC:AmiVCS/config");
#elif defined(PLATFORM_WIN)
    const char *home = getenv("USERPROFILE");
    snprintf(out, sz, "%s\\.amivcsconfig", home ? home : ".");
#else
    const char *home = getenv("HOME");
    snprintf(out, sz, "%s/.amivcsconfig", home ? home : ".");
#endif
}

int vcs_global_get(const char *key, char *out, size_t sz)
{
    char path[AC_MAX_PATH];
    vcs_global_config_path(path, sizeof(path));
    return vcs_config_file_get(path, key, out, sz);
}

int vcs_global_set(const char *key, const char *val)
{
#ifdef PLATFORM_AMIGA
    static const char *const dirs[] = { "ENV:AmiVCS", "ENVARC:AmiVCS" };
    int i, ok = 1;
    for (i = 0; i < 2; i++) {
        char path[64];
        BPTR lock = CreateDir((STRPTR)dirs[i]);
        if (lock) UnLock(lock);
        snprintf(path, sizeof(path), "%s/config", dirs[i]);
        if (!vcs_config_file_set(path, key, val)) ok = 0;
    }
    return ok;
#else
    char path[AC_MAX_PATH];
    vcs_global_config_path(path, sizeof(path));
    return vcs_config_file_set(path, key, val);
#endif
}

void vcs_global_load_options(AcConfig *cfg)
{
    AcConfig tmp;
    char path[AC_MAX_PATH];

    memset(&tmp, 0, sizeof(tmp));
    tmp.verify_ssl = cfg->verify_ssl;
    tmp.timeout = cfg->timeout;
    vcs_global_config_path(path, sizeof(path));
    if (!config_load(path, &tmp)) return;
    cfg->verify_ssl = tmp.verify_ssl;
    if (tmp.timeout > 0) cfg->timeout = tmp.timeout;
    if (tmp.charset[0]) memcpy(cfg->charset, tmp.charset, sizeof(cfg->charset));
    if (tmp.debug[0])   memcpy(cfg->debug, tmp.debug, sizeof(cfg->debug));
}

/* ------------------------------------------------------------------ */
/* per repository                                                        */
/* ------------------------------------------------------------------ */

void vcs_repo_config_path(const char *amivcs_dir, char *out, size_t sz)
{
    localfs_join(out, sz, amivcs_dir, REPO_CONFIG_FILE);
}

int vcs_repo_config_load(const char *amivcs_dir, VcsRepoConfig *rc)
{
    char path[AC_MAX_PATH];
    FILE *fp;

    memset(rc, 0, sizeof(*rc));
    vcs_repo_config_path(amivcs_dir, path, sizeof(path));
    fp = fopen(path, "r");
    if (!fp) return 0;
    fclose(fp);
    vcs_config_file_get(path, "remote", rc->remote, sizeof(rc->remote));
    vcs_config_file_get(path, "name", rc->name, sizeof(rc->name));
    vcs_config_file_get(path, "email", rc->email, sizeof(rc->email));
    vcs_config_file_get(path, "user", rc->user, sizeof(rc->user));
    vcs_config_file_get(path, "pass", rc->pass, sizeof(rc->pass));
    return 1;
}

int vcs_repo_config_set(const char *amivcs_dir, const char *key, const char *val)
{
    char path[AC_MAX_PATH];
    vcs_repo_config_path(amivcs_dir, path, sizeof(path));
    return vcs_config_file_set(path, key, val);
}

int vcs_identity(const char *amivcs_dir, char *out, size_t sz)
{
    char name[128] = "", email[128] = "";

    if (amivcs_dir) {
        char path[AC_MAX_PATH];
        vcs_repo_config_path(amivcs_dir, path, sizeof(path));
        vcs_config_file_get(path, "name", name, sizeof(name));
        vcs_config_file_get(path, "email", email, sizeof(email));
    }
    if (!name[0]) vcs_global_get("name", name, sizeof(name));
    if (!email[0]) vcs_global_get("email", email, sizeof(email));

    if (!name[0]) { if (sz) out[0] = '\0'; return 0; }
    if (email[0]) snprintf(out, sz, "%s <%s>", name, email);
    else          snprintf(out, sz, "%s", name);
    return 1;
}

/* ------------------------------------------------------------------ */
/* remote URL                                                            */
/* ------------------------------------------------------------------ */

int vcs_url_split(const char *url, char *server, size_t ssz, char *repo, size_t rsz)
{
    const char *scheme = strstr(url, "://");
    const char *host, *slash;
    size_t len = strlen(url);

    while (len > 0 && url[len - 1] == '/') len--;
    if (!scheme || scheme == url) return 0;
    host = scheme + 3;
    if ((size_t)(host - url) >= len) return 0;
    /* the last '/' before the end, but after the host */
    slash = url + len;
    while (slash > host && *(slash - 1) != '/') slash--;
    if (slash <= host) return 0;                  /* no path: no repository name */
    if ((size_t)(slash - 1 - url) >= ssz || (size_t)(url + len - slash) >= rsz) return 0;
    memcpy(server, url, (size_t)(slash - 1 - url));
    server[slash - 1 - url] = '\0';
    memcpy(repo, slash, (size_t)(url + len - slash));
    repo[url + len - slash] = '\0';
    /* both a repository name and a host before it */
    return repo[0] != '\0' && server[host - url] != '\0';
}

int vcs_remote_resolve(const char *amivcs_dir, const char *url, AcConfig *cfg,
                       char *repo_name, size_t rsz)
{
    VcsRepoConfig rc;
    const char *use;

    vcs_repo_config_load(amivcs_dir, &rc);
    use = (url && url[0]) ? url : rc.remote;
    if (!use[0]) {
        ui_notify(UI_ERROR, "amivcs: no remote URL set -- give one (amivcs push <url>) "
                            "or set it (amivcs remote <url>)");
        return 0;
    }
    if (!vcs_url_split(use, cfg->server, sizeof(cfg->server), repo_name, rsz)) {
        ui_notify(UI_ERROR, "amivcs: '%s' is not a repository URL (https://server/<name>)", use);
        return 0;
    }
    strncpy(cfg->username, rc.user, sizeof(cfg->username) - 1);
    cfg->username[sizeof(cfg->username) - 1] = '\0';
    strncpy(cfg->password, rc.pass, sizeof(cfg->password) - 1);
    cfg->password[sizeof(cfg->password) - 1] = '\0';
    return 1;
}
