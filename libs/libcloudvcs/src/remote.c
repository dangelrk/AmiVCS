/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * remote.c -- see remote.h. Talks to server/api.php exclusively
 * via libcloudhttp's generic http_request() -- never cmd.c, which is
 * AmiCloud's own server contract (module-encapsulation rule).
 *
 * Type inference note: get-object's response is raw payload bytes only,
 * no type marker (the server's X-Amivcs-Type header exists for humans
 * curling the endpoint, but http.h's HttpResponse has no general header
 * access -- see http.h). This client never actually needs it: every
 * fetch happens while walking a graph whose shape it already knows --
 * a ref always points at a commit, a commit's tree field is always a
 * tree, a tree entry's is_dir flag says blob or tree -- so the expected
 * type is always known from context before the request is even made.
 */
#include "remote.h"
#include "merge.h"
#include "treeutil.h"
#include "oidset.h"
#include "localfs.h"
#include "http.h"
#include "parson.h"
#include "md5.h"
#include "progress.h"
#include "ui_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Git-flavoured live progress line: "\rLabel: N" (total unknown, e.g.
 * pull/clone's on-demand fetch walk, whose full size isn't known ahead
 * of time -- the server is dumb and has no "object count" endpoint) or
 * "\rLabel: nn% (done/total)" when total is known upfront (push's
 * local-only object-graph walk always knows n before any network call).
 * Gated by ac_console_progress (progress.h) exactly like every other
 * console progress output in this tree, so a future GUI frontend can
 * suppress it and render its own widget instead. final appends ", done.\n". */
static void progress_line(const char *label, long done, long total, int final)
{
    if (!ac_console_progress) return;
    if (total > 0) {
        long pct = (done * 100) / total;
        printf("\r%s: %3ld%% (%ld/%ld)", label, pct, done, total);
    } else {
        printf("\r%s: %ld", label, done);
    }
    if (final) printf(", done.\n");
    fflush(stdout);
}

#define REMOTE_CONFIG_FILE "config"
#define HAS_BATCH 100

/* ------------------------------------------------------------------ */
/* <amivcs_dir>/config -- just enough to remember which server-side repo */
/* this working copy is linked to (plan section 8's ".amivcs/config").   */

static int load_remote_repo(const char *amivcs_dir, char *out, size_t sz)
{
    char path[1024];
    FILE *fp;
    char line[300];

    out[0] = '\0';
    localfs_join(path, sizeof(path), amivcs_dir, REMOTE_CONFIG_FILE);
    fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        char key[64], val[256];
        if (sscanf(line, " %63[^= \t] = %255[^\n]", key, val) == 2 && strcmp(key, "remote_repo") == 0) {
            strncpy(out, val, sz - 1);
            out[sz - 1] = '\0';
        }
    }
    fclose(fp);
    return out[0] != '\0';
}

static int save_remote_repo(const char *amivcs_dir, const char *repo_name)
{
    char path[1024];
    FILE *fp;

    localfs_join(path, sizeof(path), amivcs_dir, REMOTE_CONFIG_FILE);
    fp = fopen(path, "w");
    if (!fp) return 0;
    fprintf(fp, "remote_repo=%s\n", repo_name);
    fclose(fp);
    return 1;
}

/* Resolves repo_name (if given, remembering it) or falls back to
 * .amivcs/config's remembered remote. Returns 1 on success. */
static int resolve_repo_name(VcsRepo *repo, const char *repo_name, char *out, size_t sz)
{
    if (repo_name && repo_name[0]) {
        strncpy(out, repo_name, sz - 1);
        out[sz - 1] = '\0';
        save_remote_repo(repo->amivcs_dir, out);
        return 1;
    }
    if (load_remote_repo(repo->amivcs_dir, out, sz)) return 1;
    ui_notify(UI_ERROR, "amivcs: no remote repository configured -- give one explicitly");
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP + JSON plumbing.                                                 */

static void build_url(const AcConfig *cfg, char *out, size_t sz, const char *query)
{
    snprintf(out, sz, "%s/api.php?%s", cfg->server, query);
}

static void build_auth(const AcConfig *cfg, char *out, size_t sz)
{
    snprintf(out, sz, "%s:%s", cfg->username, cfg->password);
    /* Every request builds its credentials here first -- the one spot
     * that sees the config right before talking to the server. */
    http_set_timeout(cfg->timeout);
}

/* One request, repeated up to REQUEST_TRIES times while it fails at the
 * transport level (no response, or a body that broke off -- status 0)
 * and retry is allowed. Over a modem a single dropped connection among
 * the thousands of requests of a clone is to be expected; the HTTP layer
 * already fails only after its idle timeout, so no extra pause here.
 * Never repeated: a Ctrl-C/cancel, and requests whose repetition could
 * do harm -- update-ref is a compare-and-swap that would report a false
 * conflict if the first attempt went through but its answer got lost.
 * Every other API action is idempotent (objects are content-addressed). */
#define REQUEST_TRIES 3

static HttpResponse *api_request(const AcConfig *cfg, const char *method, const char *query,
                                 const void *data, long len, int retry)
{
    char url[2048], auth[256];
    HttpResponse *resp = NULL;
    int attempt;

    build_url(cfg, url, sizeof(url), query);
    build_auth(cfg, auth, sizeof(auth));

    for (attempt = 0; attempt < (retry ? REQUEST_TRIES : 1); attempt++) {
        http_response_free(resp);
        resp = http_request(method, url, auth, (const char *)data, len, cfg->verify_ssl);
        if (resp && (resp->status_code != 0 || resp->aborted)) break;
        if (ui_break_pending()) break;
        if (retry && attempt + 1 < REQUEST_TRIES)
            ui_notify(UI_WARN, "amivcs: network error, retrying (%d/%d)", attempt + 2, REQUEST_TRIES);
    }
    if (resp && (resp->status_code == 0 || resp->aborted)) {
        if (!resp->aborted) ui_notify(UI_ERROR, "amivcs: network error");
        http_response_free(resp);
        return NULL;
    }
    if (!resp) ui_notify(UI_ERROR, "amivcs: network error");
    return resp;
}

static int json_is_ok(JSON_Object *obj)
{
    const char *s = json_object_get_string(obj, "status");
    return s && strcmp(s, "ok") == 0;
}

static void report_json_error(JSON_Object *obj, const char *fallback)
{
    const char *msg = json_object_get_string(obj, "message");
    ui_notify(UI_ERROR, "amivcs: %s", msg ? msg : fallback);
}

/* GET query, parse the JSON body. *out_val must be freed with
 * json_value_free() by the caller on success. Returns 1 on success
 * (regardless of the JSON envelope's own status -- callers check
 * json_is_ok() themselves), 0 on a transport/parse failure. retry: see
 * api_request(). */
static int api_get_json(const AcConfig *cfg, const char *query, JSON_Value **out_val, JSON_Object **out_obj,
                        int retry)
{
    HttpResponse *resp;
    JSON_Value *val;

    resp = api_request(cfg, "GET", query, NULL, 0, retry);
    if (!resp) return 0;

    val = resp->body ? json_parse_string(resp->body) : NULL;
    http_response_free(resp);
    if (!val) { ui_notify(UI_ERROR, "amivcs: malformed server response"); return 0; }

    *out_obj = json_value_get_object(val);
    if (!*out_obj) { json_value_free(val); ui_notify(UI_ERROR, "amivcs: malformed server response"); return 0; }
    *out_val = val;
    return 1;
}

/* POST upload_data, parse the JSON body -- same contract as api_get_json();
 * the only POST is put-object, which is idempotent, so always retried. */
static int api_post_json(const AcConfig *cfg, const char *query, const void *data, long len,
                         JSON_Value **out_val, JSON_Object **out_obj)
{
    HttpResponse *resp;
    JSON_Value *val;

    resp = api_request(cfg, "POST", query, data, len, 1);
    if (!resp) return 0;

    val = resp->body ? json_parse_string(resp->body) : NULL;
    http_response_free(resp);
    if (!val) { ui_notify(UI_ERROR, "amivcs: malformed server response"); return 0; }

    *out_obj = json_value_get_object(val);
    if (!*out_obj) { json_value_free(val); ui_notify(UI_ERROR, "amivcs: malformed server response"); return 0; }
    *out_val = val;
    return 1;
}

/* GET, expecting a raw object payload (get-object) rather than JSON.
 * *out_data is malloc'd (via http.h's HttpResponse -- copied out before
 * freeing it), caller frees. Returns 1 on success, 0 on transport error
 * or a non-2xx status (the body would be a JSON error envelope instead,
 * which this doesn't try to parse -- callers only use this once they
 * already know the object should exist, e.g. right after action=has). */
static int api_get_raw(const AcConfig *cfg, const char *query, void **out_data, long *out_len)
{
    HttpResponse *resp;

    resp = api_request(cfg, "GET", query, NULL, 0, 1);
    if (!resp) return 0;
    if (resp->status_code < 200 || resp->status_code >= 300 || !resp->body) {
        ui_notify(UI_ERROR, "amivcs: object fetch failed (HTTP %d)", resp->status_code);
        http_response_free(resp);
        return 0;
    }

    *out_data = malloc(resp->body_len ? (size_t)resp->body_len : 1);
    if (!*out_data) { http_response_free(resp); return 0; }
    memcpy(*out_data, resp->body, (size_t)resp->body_len);
    *out_len = resp->body_len;
    http_response_free(resp);
    return 1;
}

/* ------------------------------------------------------------------ */

int vcs_remote_create_repo(const AcConfig *cfg, const char *repo_name)
{
    char query[300];
    JSON_Value *val;
    JSON_Object *obj;
    int ok;

    snprintf(query, sizeof(query), "action=create-repo&repo=%s", repo_name);
    if (!api_get_json(cfg, query, &val, &obj, 0)) return 0;
    ok = json_is_ok(obj);
    if (!ok) report_json_error(obj, "create-repo failed");
    json_value_free(val);
    return ok;
}

static int fetch_remote_refs(const AcConfig *cfg, const char *repo_name, VcsRefList *out)
{
    char query[300];
    JSON_Value *val;
    JSON_Object *obj;
    JSON_Array *arr;
    size_t i, n;

    out->items = NULL;
    out->n = out->cap = 0;

    snprintf(query, sizeof(query), "action=refs&repo=%s", repo_name);
    if (!api_get_json(cfg, query, &val, &obj, 1)) return 0;
    if (!json_is_ok(obj)) { report_json_error(obj, "refs request failed"); json_value_free(val); return 0; }

    arr = json_object_get_array(obj, "refs");
    n = arr ? json_array_get_count(arr) : 0;
    for (i = 0; i < n; i++) {
        JSON_Object *e = json_array_get_object(arr, i);
        const char *name = e ? json_object_get_string(e, "name") : NULL;
        const char *oidhex = e ? json_object_get_string(e, "oid") : NULL;
        VcsOid oid;
        if (!name || !oidhex || !vcs_oid_from_hex(oidhex, &oid)) continue;
        vcs_refs_upsert(out, name, &oid);
    }
    json_value_free(val);
    return 1;
}

/* Fetches root (of known type -- see the file header note) and everything
 * reachable from it that isn't present locally yet. Walks the graph with
 * an explicit work stack instead of recursion: the walk follows parent
 * links, so a recursive version went one stack frame deeper per commit
 * of linear history -- about a hundred commits overran the Amiga's
 * 64 KB stack (vbcc does no stack checking, so that crashed). visited
 * avoids re-walking shared subtrees/history. Objects already present
 * locally are still walked: an interrupted clone/pull stores an object
 * before its children, so a local commit doesn't mean a complete one.
 * *fetched is bumped once per actual network fetch and drives the live
 * "Receiving objects: N" progress line -- there's no upfront total
 * here (the dumb server has no "how many objects total" endpoint), so
 * this is a running count, not a percentage.
 *
 * *cancelled is set the moment ui_progress() returns 0 -- a GUI
 * frontend's cancel button, or Ctrl-C in the CLI. Same pattern as
 * core/common/panel.c's CopyCtx.cancelled. */
typedef struct {
    VcsOid     oid;
    VcsObjType type;
} FetchItem;

typedef struct {
    FetchItem *items;
    int        n, cap;
} FetchStack;

static int fetch_push(FetchStack *st, const VcsOid *oid, VcsObjType type)
{
    if (vcs_oid_is_zero(oid)) return 1;
    if (st->n == st->cap) {
        int ncap = st->cap ? st->cap * 2 : 64;
        FetchItem *ni = (FetchItem *)realloc(st->items, (size_t)ncap * sizeof(FetchItem));
        if (!ni) { ui_notify(UI_ERROR, "amivcs: out of memory"); return 0; }
        st->items = ni;
        st->cap = ncap;
    }
    st->items[st->n].oid = *oid;
    st->items[st->n].type = type;
    st->n++;
    return 1;
}

static int fetch_objects(VcsRepo *repo, const AcConfig *cfg, const char *repo_name,
                         const VcsOid *root, VcsObjType root_type, VcsOidSet *visited,
                         long *fetched, int *cancelled)
{
    FetchStack st;
    int ok;

    st.items = NULL;
    st.n = st.cap = 0;
    ok = fetch_push(&st, root, root_type);

    while (ok && st.n > 0) {
        FetchItem it = st.items[--st.n];
        void *data;
        long len;

        if (*cancelled) { ok = 0; break; }
        if (vcs_oidset_contains(visited, &it.oid)) continue;
        vcs_oidset_add(visited, &it.oid);

        if (vcsobjstore_exists(&repo->store, &it.oid)) {
            VcsObjType type;
            if (!vcsobjstore_read(&repo->store, &it.oid, &type, &data, &len)) { ok = 0; break; }
        } else {
            char hex[VCS_OID_HEX], query[300];
            VcsOid got_oid, stored_oid;
            char hex_check[VCS_OID_HEX];

            vcs_oid_to_hex(&it.oid, hex);
            snprintf(query, sizeof(query), "action=get-object&repo=%s&oid=%s", repo_name, hex);
            if (!api_get_raw(cfg, query, &data, &len)) {
                if (ui_break_pending()) *cancelled = 1;
                ok = 0;
                break;
            }

            md5_buffer_hex(data, (size_t)len, hex_check);
            if (!vcs_oid_from_hex(hex_check, &got_oid) || !vcs_oid_equal(&got_oid, &it.oid)) {
                ui_notify(UI_ERROR, "amivcs: fetched object %s doesn't match its content (corrupt transfer?)", hex);
                free(data);
                ok = 0;
                break;
            }
            if (!vcsobjstore_write(&repo->store, it.type, data, len, &stored_oid)) { free(data); ok = 0; break; }

            (*fetched)++;
            progress_line("Receiving objects", *fetched, 0, 0);
            if (!ui_progress(*fetched, -1, "Receiving objects")) { *cancelled = 1; free(data); ok = 0; break; }
        }

        switch (it.type) {
        case VCS_OBJ_COMMIT: {
            VcsCommit c;
            int i;
            vcs_commit_init(&c);
            if (vcs_commit_decode(data, len, &c)) {
                /* Pushed last = walked first: the tree before history. */
                for (i = c.nparents - 1; ok && i >= 0; i--)
                    ok = fetch_push(&st, &c.parents[i], VCS_OBJ_COMMIT);
                if (ok) ok = fetch_push(&st, &c.tree, VCS_OBJ_TREE);
            }
            vcs_commit_free(&c);
            break;
        }
        case VCS_OBJ_TREE: {
            VcsTree tree;
            int i;
            vcs_tree_init(&tree);
            if (vcs_tree_decode(data, len, &tree)) {
                for (i = tree.n - 1; ok && i >= 0; i--)
                    ok = fetch_push(&st, &tree.items[i].oid,
                                    tree.items[i].is_dir ? VCS_OBJ_TREE : VCS_OBJ_BLOB);
            }
            vcs_tree_free(&tree);
            break;
        }
        case VCS_OBJ_BLOB:
            break;
        }
        free(data);
    }

    free(st.items);
    return ok;
}

/* <amivcs_dir>/clone-pending holds the repository name while a clone is
 * still fetching. If the clone breaks off (modem line dropped, Ctrl-C),
 * running the same clone again finds it and resumes -- every object
 * already fetched stays, fetch_objects() only loads what's missing --
 * instead of failing with "already an AMIVCS repository". */
#define CLONE_PENDING_FILE "clone-pending"

static int clone_pending(const char *amivcs_dir, const char *repo_name)
{
    char path[1024], line[300];
    FILE *fp;
    int match = 0;

    localfs_join(path, sizeof(path), amivcs_dir, CLONE_PENDING_FILE);
    fp = fopen(path, "r");
    if (!fp) return 0;
    if (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        match = strcmp(line, repo_name) == 0;
    }
    fclose(fp);
    return match;
}

static void set_clone_pending(const char *amivcs_dir, const char *repo_name)
{
    char path[1024];
    FILE *fp;

    localfs_join(path, sizeof(path), amivcs_dir, CLONE_PENDING_FILE);
    if (!repo_name) { remove(path); return; }
    fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "%s\n", repo_name);
    fclose(fp);
}

int vcs_remote_clone(const AcConfig *cfg, const char *repo_name, const char *local_dir)
{
    VcsRefList remote_refs;
    VcsRepo repo;
    VcsOidSet visited;
    long fetched = 0;
    int cancelled = 0;
    int i, main_idx = -1;

    if (!localfs_mkdir_p(local_dir)) {
        ui_notify(UI_ERROR, "amivcs: cannot create '%s'", local_dir);
        return 0;
    }
    if (!vcs_repo_init(local_dir)) {
        if (!vcs_repo_open(&repo, local_dir) || !clone_pending(repo.amivcs_dir, repo_name)) {
            ui_notify(UI_ERROR, "amivcs: '%s' is already an AMIVCS repository", local_dir);
            return 0;
        }
        ui_notify(UI_INFO, "Resuming interrupted clone of '%s'.", repo_name);
    } else {
        if (!vcs_repo_open(&repo, local_dir)) return 0;
        set_clone_pending(repo.amivcs_dir, repo_name);
    }

    if (!fetch_remote_refs(cfg, repo_name, &remote_refs)) return 0;
    if (remote_refs.n == 0) {
        ui_notify(UI_INFO, "Cloned empty repository '%s'.", repo_name);
        vcs_refs_free(&remote_refs);
        save_remote_repo(repo.amivcs_dir, repo_name);
        set_clone_pending(repo.amivcs_dir, NULL);
        return 1;
    }

    vcs_oidset_init(&visited);
    for (i = 0; i < remote_refs.n; i++) {
        if (strcmp(remote_refs.items[i].name, VCS_REF_PREFIX "main") == 0) main_idx = i;
        if (!fetch_objects(&repo, cfg, repo_name, &remote_refs.items[i].oid, VCS_OBJ_COMMIT, &visited, &fetched, &cancelled)) {
            vcs_oidset_free(&visited);
            vcs_refs_free(&remote_refs);
            ui_notify(UI_WARN, "amivcs: clone %s -- run the same clone again to resume",
                      cancelled ? "cancelled" : "interrupted");
            return 0;
        }
    }
    vcs_oidset_free(&visited);
    if (fetched > 0) progress_line("Receiving objects", fetched, 0, 1);

    {
        VcsRefList local_refs;
        vcs_refs_load(repo.amivcs_dir, &local_refs);
        for (i = 0; i < remote_refs.n; i++) {
            vcs_refs_upsert(&local_refs, remote_refs.items[i].name, &remote_refs.items[i].oid);
        }
        vcs_refs_save(repo.amivcs_dir, &local_refs);
        vcs_refs_free(&local_refs);
    }

    if (main_idx < 0) main_idx = 0; /* no "main" -- fall back to whatever the server listed first */

    {
        const char *checkout_branch = remote_refs.items[main_idx].name + strlen(VCS_REF_PREFIX);
        VcsOid tree_oid;

        if (!vcs_commit_tree(&repo.store, &remote_refs.items[main_idx].oid, &tree_oid)) { vcs_refs_free(&remote_refs); return 0; }
        if (!vcs_repo_materialize_tree(&repo, &tree_oid)) { vcs_refs_free(&remote_refs); return 0; }
        vcs_head_set_branch(repo.amivcs_dir, checkout_branch);
        ui_notify(UI_INFO, "Cloned '%s' into %s, checked out '%s'.", repo_name, local_dir, checkout_branch);
    }

    save_remote_repo(repo.amivcs_dir, repo_name);
    set_clone_pending(repo.amivcs_dir, NULL);
    vcs_refs_free(&remote_refs);
    return 1;
}

/* Creates repo_name on the server if it doesn't exist yet -- called only
 * when the caller explicitly named a repo (first-time push linking a
 * local repo to a new server-side one). Tolerates 409 (already exists,
 * e.g. re-linking or a second push after the first created it); any
 * other failure is fatal. */
static int ensure_repo_exists(const AcConfig *cfg, const char *repo_name)
{
    char query[300];
    JSON_Value *val;
    JSON_Object *obj;
    int ok;

    snprintf(query, sizeof(query), "action=create-repo&repo=%s", repo_name);
    if (!api_get_json(cfg, query, &val, &obj, 1)) return 0;
    ok = json_is_ok(obj) || (int)json_object_get_number(obj, "code") == 409;
    if (!ok) report_json_error(obj, "create-repo failed");
    json_value_free(val);
    return ok;
}

/* True if ancestor is at or before descendant in commit history (a BFS
 * over parent links, exactly like merge.c's collect_ancestors -- kept as
 * its own small copy rather than shared, since merge.c's version returns
 * a whole ancestor set and this only needs a yes/no answer without
 * building one). vcs_oid_is_zero(ancestor) is vacuously true (an empty
 * branch predates everything). */
static int is_ancestor_or_equal(const VcsObjStore *store, const VcsOid *ancestor, const VcsOid *descendant)
{
    VcsOidSet queue, seen;
    int qi = 0, found;

    if (vcs_oid_is_zero(ancestor) || vcs_oid_equal(ancestor, descendant)) return 1;

    vcs_oidset_init(&queue);
    vcs_oidset_init(&seen);
    vcs_oidset_add(&queue, descendant);
    found = 0;

    while (qi < queue.n && !found) {
        VcsOid cur = queue.items[qi++];
        VcsObjType type;
        void *data;
        long len;

        if (vcs_oidset_contains(&seen, &cur)) continue;
        vcs_oidset_add(&seen, &cur);
        if (vcs_oid_equal(&cur, ancestor)) { found = 1; break; }

        if (vcsobjstore_read(store, &cur, &type, &data, &len)) {
            VcsCommit c;
            int j;
            vcs_commit_init(&c);
            if (type == VCS_OBJ_COMMIT && vcs_commit_decode(data, len, &c)) {
                for (j = 0; j < c.nparents; j++) vcs_oidset_add(&queue, &c.parents[j]);
            }
            free(data);
            vcs_commit_free(&c);
        }
    }
    vcs_oidset_free(&queue);
    vcs_oidset_free(&seen);
    return found;
}

int vcs_remote_push(VcsRepo *repo, const AcConfig *cfg, const char *repo_name)
{
    char resolved_name[256];
    VcsHead head;
    VcsOid local_commit;
    VcsOidSet reachable;
    VcsOid *oids = NULL;
    VcsObjType *types = NULL;
    int i, n = 0;
    char *have_flags = NULL;
    VcsRefList remote_refs;
    char fullname[192];
    char remote_old_hex[VCS_OID_HEX];

    if (!resolve_repo_name(repo, repo_name, resolved_name, sizeof(resolved_name))) return 0;
    if (repo_name && repo_name[0] && !ensure_repo_exists(cfg, resolved_name)) return 0;

    if (!vcs_head_load(repo->amivcs_dir, &head)) return 0;
    if (head.detached) { ui_notify(UI_ERROR, "amivcs: HEAD is detached -- switch to a branch first"); return 0; }
    if (!vcs_head_resolve(repo->amivcs_dir, &local_commit)) return 0;
    if (vcs_oid_is_zero(&local_commit)) { ui_notify(UI_ERROR, "amivcs: nothing to push -- no commits yet"); return 0; }

    /* Fast-forward safety check -- MUST happen here, client-side: the
     * server's update-ref is a plain compare-and-swap on the ref's
     * current value (see server/includes/RefStore.php), it has no
     * concept of commit ancestry at all (ADR 09: the server carries no
     * VCS logic whatsoever). Nothing server-side would ever catch a
     * diverged push -- skipping this check would let a push silently
     * overwrite someone else's commits instead of being refused. */
    snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, head.branch);
    if (!fetch_remote_refs(cfg, resolved_name, &remote_refs)) return 0;
    {
        VcsRef *r = vcs_refs_find(&remote_refs, fullname);
        VcsOid remote_oid = r ? r->oid : VCS_OID_ZERO;
        vcs_refs_free(&remote_refs);
        if (!is_ancestor_or_equal(&repo->store, &remote_oid, &local_commit)) {
            ui_notify(UI_ERROR, "amivcs: rejected -- the server has commits you don't have; pull and merge first");
            return 0;
        }
    }

    /* Collect every object reachable from local_commit (full history --
     * v1 doesn't track "already pushed up to here", it just asks the
     * server via action=has and skips what it already reports having). */
    vcs_oidset_init(&reachable);
    {
        VcsOidSet queue;
        int qi = 0;
        vcs_oidset_init(&queue);
        vcs_oidset_add(&queue, &local_commit);

        while (qi < queue.n) {
            VcsOid cur = queue.items[qi++];
            VcsObjType type;
            void *data;
            long len;

            if (vcs_oidset_contains(&reachable, &cur)) continue;
            if (!vcsobjstore_read(&repo->store, &cur, &type, &data, &len)) { vcs_oidset_free(&queue); vcs_oidset_free(&reachable); return 0; }
            vcs_oidset_add(&reachable, &cur);

            if (type == VCS_OBJ_COMMIT) {
                VcsCommit c;
                int j;
                vcs_commit_init(&c);
                if (vcs_commit_decode(data, len, &c)) {
                    vcs_oidset_add(&queue, &c.tree);
                    for (j = 0; j < c.nparents; j++) vcs_oidset_add(&queue, &c.parents[j]);
                }
                vcs_commit_free(&c);
            } else if (type == VCS_OBJ_TREE) {
                VcsTree tree;
                int j;
                vcs_tree_init(&tree);
                if (vcs_tree_decode(data, len, &tree)) {
                    for (j = 0; j < tree.n; j++) vcs_oidset_add(&queue, &tree.items[j].oid);
                }
                vcs_tree_free(&tree);
            }
            free(data);
        }
        vcs_oidset_free(&queue);
    }

    n = reachable.n;
    oids = reachable.items; /* borrow -- freed via vcs_oidset_free below */
    progress_line("Counting objects", n, 0, 1);
    types = (VcsObjType *)malloc(sizeof(VcsObjType) * (size_t)(n ? n : 1));
    have_flags = (char *)calloc((size_t)(n ? n : 1), 1);
    if (!types || !have_flags) { free(types); free(have_flags); vcs_oidset_free(&reachable); return 0; }

    for (i = 0; i < n; i++) {
        VcsObjType type;
        void *data;
        long len;
        if (!vcsobjstore_read(&repo->store, &oids[i], &type, &data, &len)) { free(types); free(have_flags); vcs_oidset_free(&reachable); return 0; }
        types[i] = type;
        free(data);
    }

    /* action=has negotiation, batched to keep query strings short. */
    for (i = 0; i < n; i += HAS_BATCH) {
        int batch_n = (n - i) > HAS_BATCH ? HAS_BATCH : (n - i);
        char query[64 + HAS_BATCH * 34];
        int pos, j;
        JSON_Value *val;
        JSON_Object *obj;

        pos = snprintf(query, sizeof(query), "action=has&repo=%s&oids=", resolved_name);
        for (j = 0; j < batch_n; j++) {
            char hex[VCS_OID_HEX];
            vcs_oid_to_hex(&oids[i + j], hex);
            pos += snprintf(query + pos, sizeof(query) - (size_t)pos, "%s%s", j ? "," : "", hex);
        }

        if (!api_get_json(cfg, query, &val, &obj, 1)) { free(types); free(have_flags); vcs_oidset_free(&reachable); return 0; }
        if (!json_is_ok(obj)) { report_json_error(obj, "has request failed"); json_value_free(val); free(types); free(have_flags); vcs_oidset_free(&reachable); return 0; }

        {
            JSON_Array *arr = json_object_get_array(obj, "have");
            size_t hn = arr ? json_array_get_count(arr) : 0, hi;
            for (hi = 0; hi < hn; hi++) {
                const char *hex = json_array_get_string(arr, hi);
                VcsOid have_oid;
                if (hex && vcs_oid_from_hex(hex, &have_oid)) {
                    int j2;
                    for (j2 = 0; j2 < batch_n; j2++) {
                        if (vcs_oid_equal(&oids[i + j2], &have_oid)) { have_flags[i + j2] = 1; break; }
                    }
                }
            }
        }
        progress_line("Checking objects", i + batch_n, n, i + batch_n >= n);
        json_value_free(val);

        if (!ui_progress(i + batch_n, n, "Checking objects")) {
            free(types); free(have_flags); vcs_oidset_free(&reachable);
            ui_notify(UI_WARN, "amivcs: push cancelled");
            return 0;
        }
    }

    /* Upload everything the server reported missing. */
    {
        long to_upload = 0, uploaded = 0;
        for (i = 0; i < n; i++) if (!have_flags[i]) to_upload++;

        for (i = 0; i < n; i++) {
            VcsObjType type;
            void *data;
            long len;
            char hex[VCS_OID_HEX], query[300];
            JSON_Value *val;
            JSON_Object *obj;

            if (have_flags[i]) continue;

            if (!vcsobjstore_read(&repo->store, &oids[i], &type, &data, &len)) { free(types); free(have_flags); vcs_oidset_free(&reachable); return 0; }
            vcs_oid_to_hex(&oids[i], hex);
            snprintf(query, sizeof(query), "action=put-object&repo=%s&oid=%s&type=%d", resolved_name, hex, (int)type);

            if (!api_post_json(cfg, query, data, len, &val, &obj)) { free(data); free(types); free(have_flags); vcs_oidset_free(&reachable); return 0; }
            if (!json_is_ok(obj)) {
                report_json_error(obj, "put-object failed");
                json_value_free(val); free(data); free(types); free(have_flags); vcs_oidset_free(&reachable);
                return 0;
            }
            json_value_free(val);
            free(data);

            uploaded++;
            progress_line("Writing objects", uploaded, to_upload, uploaded >= to_upload);

            if (!ui_progress(uploaded, to_upload, "Writing objects")) {
                free(types); free(have_flags); vcs_oidset_free(&reachable);
                ui_notify(UI_WARN, "amivcs: push cancelled");
                return 0;
            }
        }
    }

    free(types);
    free(have_flags);
    vcs_oidset_free(&reachable); /* also frees oids, which aliased reachable.items */

    /* Ref update, CAS against whatever the server currently has. */
    if (!fetch_remote_refs(cfg, resolved_name, &remote_refs)) return 0;
    snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, head.branch);
    remote_old_hex[0] = '\0';
    {
        VcsRef *r = vcs_refs_find(&remote_refs, fullname);
        if (r) vcs_oid_to_hex(&r->oid, remote_old_hex);
    }
    vcs_refs_free(&remote_refs);

    {
        char hex[VCS_OID_HEX], query[400];
        JSON_Value *val;
        JSON_Object *obj;
        int ok;

        vcs_oid_to_hex(&local_commit, hex);
        snprintf(query, sizeof(query), "action=update-ref&repo=%s&name=%s&old=%s&new=%s",
                resolved_name, fullname, remote_old_hex, hex);

        /* No retry: a compare-and-swap, see api_request(). */
        if (!api_get_json(cfg, query, &val, &obj, 0)) return 0;
        ok = json_is_ok(obj);
        if (!ok) {
            report_json_error(obj, "update-ref failed");
            ui_notify(UI_ERROR, "amivcs: push rejected -- pull and merge first");
        } else {
            ui_notify(UI_INFO, "Pushed '%s' to '%s'.", head.branch, resolved_name);
        }
        json_value_free(val);
        return ok;
    }
}

int vcs_remote_pull(VcsRepo *repo, const AcConfig *cfg, const char *repo_name, const char *branch)
{
    char resolved_name[256];
    VcsHead head;
    const char *branch_name;
    VcsRefList remote_refs;
    char fullname[192];
    VcsRef *r;
    VcsOid theirs_commit;
    VcsOidSet visited;
    long fetched = 0;
    int cancelled = 0;
    int ok;
    char label[256];

    if (!resolve_repo_name(repo, repo_name, resolved_name, sizeof(resolved_name))) return 0;

    if (branch && branch[0]) {
        branch_name = branch;
    } else {
        if (!vcs_head_load(repo->amivcs_dir, &head)) return 0;
        if (head.detached) { ui_notify(UI_ERROR, "amivcs: HEAD is detached -- switch to a branch first"); return 0; }
        branch_name = head.branch;
    }

    if (!fetch_remote_refs(cfg, resolved_name, &remote_refs)) return 0;
    snprintf(fullname, sizeof(fullname), "%s%s", VCS_REF_PREFIX, branch_name);
    r = vcs_refs_find(&remote_refs, fullname);
    if (!r) {
        ui_notify(UI_ERROR, "amivcs: no such remote branch '%s'", branch_name);
        vcs_refs_free(&remote_refs);
        return 0;
    }
    theirs_commit = r->oid;
    vcs_refs_free(&remote_refs);

    vcs_oidset_init(&visited);
    ok = fetch_objects(repo, cfg, resolved_name, &theirs_commit, VCS_OBJ_COMMIT, &visited, &fetched, &cancelled);
    vcs_oidset_free(&visited);
    if (!ok) {
        if (cancelled) ui_notify(UI_WARN, "amivcs: pull cancelled");
        return 0;
    }
    if (fetched > 0) progress_line("Receiving objects", fetched, 0, 1);

    snprintf(label, sizeof(label), "%s/%s", resolved_name, branch_name);
    return vcs_repo_merge_commit(repo, &theirs_commit, label);
}
