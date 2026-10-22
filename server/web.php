<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
define('AMIVCS_SERVER', 1);
require __DIR__ . '/config.php';

/* Weboberflaeche (docs/design/webui-plan.md): eigene und oeffentliche
 * Repositories ansehen -- Code, Historie mit Graph, Commits mit Diff,
 * Branches --, Repo-Einstellungen, persoenliche Einstellungen mit
 * App-Passwoertern, Benutzerverwaltung. Rein lesend gegenueber dem
 * Repository-Inhalt: Commits entstehen nur auf dem Client (ADR 09/10).
 *
 * GET  ?view=repos|explore|settings|users|login
 * GET  ?r=<besitzer>/<repo>&v=code|blob|raw|zip|history|commit|branches|settings
 *      [&ref=<branch|commit>][&p=<pfad>][&c=<commit>][&page=n][&dl=1]
 * POST do=... (immer mit CSRF-Token) */

date_default_timezone_set(defined('AMIVCS_TIMEZONE') ? AMIVCS_TIMEZONE : 'Europe/Berlin');

/* ── HTTPS enforce (AMIVCS_ALLOW_HTTP: nur lokale Entwicklung) ─────────
 * Vor session_start(), damit nie ein Session-Cookie ueber HTTP entsteht. */
$https = !empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off';
if (!$https && !(defined('AMIVCS_ALLOW_HTTP') && AMIVCS_ALLOW_HTTP)) {
    header('Location: https://' . $_SERVER['HTTP_HOST'] . $_SERVER['REQUEST_URI']);
    exit;
}
session_set_cookie_params([
    'lifetime' => 0,
    'path'     => '/',
    'secure'   => $https,
    'httponly' => true,
    'samesite' => 'Lax',  /* oeffentliche Repo-Links kommen oft von fremden Seiten */
]);
session_start();

require __DIR__ . '/includes/Version.php';
require __DIR__ . '/includes/Response.php';
require __DIR__ . '/includes/DB.php';
require __DIR__ . '/includes/Throttle.php';
require __DIR__ . '/includes/Users.php';
require __DIR__ . '/includes/AppPasswords.php';
require __DIR__ . '/includes/Auth.php';
require __DIR__ . '/includes/Repos.php';
require __DIR__ . '/includes/ObjectDecoder.php';
require __DIR__ . '/includes/GraphLayout.php';
require __DIR__ . '/includes/Diff.php';
require __DIR__ . '/includes/RepoReader.php';
require __DIR__ . '/includes/web/helpers.php';
require __DIR__ . '/includes/web/layout.php';
require __DIR__ . '/includes/web/repos.php';
require __DIR__ . '/includes/web/repo.php';
require __DIR__ . '/includes/web/settings.php';
require __DIR__ . '/includes/web/users.php';

header('X-Content-Type-Options: nosniff');
header('Referrer-Policy: same-origin');
header('X-Frame-Options: SAMEORIGIN');

if (empty($_SESSION['csrf'])) {
    $_SESSION['csrf'] = bin2hex(random_bytes(16));
}
$isPost = $_SERVER['REQUEST_METHOD'] === 'POST';
$do     = (string)($_POST['do'] ?? '');

if ($isPost && !hash_equals($_SESSION['csrf'], (string)($_POST['csrf'] ?? ''))) {
    flash('Die Sitzung ist abgelaufen. Bitte erneut versuchen.', 'error');
    redirect('/web.php');
}

/* ── Anmelden ────────────────────────────────────────────────────────── */
if ($isPost && $do === 'login') {
    $r = Auth::webLogin((string)($_POST['username'] ?? ''), (string)($_POST['password'] ?? ''));
    if (is_array($r)) {
        Auth::startSession($r);
        redirect('/web.php');
    }
    renderLogin($r);
    exit;
}

$loggedIn = Auth::loadSessionUser();
$uid      = Auth::$user_id;

function requireLogin(): void
{
    global $loggedIn;
    if (!$loggedIn) {
        renderLogin('');
        exit;
    }
}

function requireAdmin(): void
{
    requireLogin();
    if (!Auth::$is_admin) {
        http_response_code(403);
        exit('Nur für Administratoren.');
    }
}

function notFound(string $what = 'Diese Seite gibt es nicht.'): void
{
    http_response_code(404);
    $hint = Auth::$user_id === 0
        ? '<span>Private Repositories sind nur nach dem <a href="' . h(webUrl(['view' => 'login'])) . '">Anmelden</a> für ihren Eigentümer sichtbar.</span>'
        : '';
    renderLayout('Nicht gefunden', '', '<div class="empty card">' . icon('search', 'ic empty-ic')
        . '<strong>' . h($what) . '</strong>' . $hint . '</div>');
    exit;
}

/* Datei ausliefern. Inline nur Text (als text/plain) und PNG/JPEG/GIF,
 * alles andere als Download; nie HTML/SVG unter der eigenen Origin. */
function sendFile(string $file, string $name, string $mime, bool $inline): void
{
    $ascii = preg_replace('/[^\x20-\x7e]|["\\\\]/', '_', $name);
    header('Content-Type: ' . $mime);
    header('Content-Length: ' . filesize($file));
    header('Content-Disposition: ' . ($inline ? 'inline' : 'attachment') . '; filename="' . $ascii
           . '"; filename*=UTF-8\'\'' . rawurlencode($name));
    header("Content-Security-Policy: sandbox; default-src 'none'; img-src 'self'");
    readfile($file);
    exit;
}

/* Repo eines POST-Formulars, nur fuer den Eigentuemer */
function ownRepoFromPost(): array
{
    $parts = explode('/', (string)($_POST['r'] ?? ''), 2);
    $repo  = count($parts) === 2 ? Repos::forView($parts[0], $parts[1]) : null;
    if ($repo === null || !$repo['is_owner']) {
        notFound();
    }
    return $repo;
}

/* ── POST-Aktionen (nur angemeldet) ──────────────────────────────────── */
if ($isPost) {
    requireLogin();
    switch ($do) {
    case 'logout':
        $_SESSION = [];
        session_destroy();
        redirect('/web.php?view=explore');

    case 'repo_create':
        $name = trim((string)($_POST['name'] ?? ''));
        $err  = Repos::createFor($uid, Auth::$username, $name, trim((string)($_POST['description'] ?? '')),
                                 !empty($_POST['is_public']));
        if ($err !== null) {
            flash($err, 'error');
            redirect(webUrl());
        }
        flash("Repository $name angelegt.");
        redirect(webUrl(['r' => Auth::$username . '/' . $name]));

    case 'repo_settings':
        $repo = ownRepoFromPost();
        $err  = Repos::updateSettings((int)$repo['id'], trim((string)($_POST['description'] ?? '')),
                                      !empty($_POST['is_public']), (string)($_POST['default_branch'] ?? ''));
        flash($err ?? 'Einstellungen gespeichert.', $err ? 'error' : 'info');
        redirect(repoUrl($repo, 'settings'));

    case 'repo_delete':
        $repo = ownRepoFromPost();
        if ((string)($_POST['confirm'] ?? '') !== $repo['name']) {
            flash('Löschen abgebrochen: Der eingetippte Name stimmt nicht.', 'error');
            redirect(repoUrl($repo, 'settings'));
        }
        $err = Repos::delete($repo);
        flash($err ?? "Repository {$repo['name']} gelöscht.", $err ? 'error' : 'info');
        redirect(webUrl());

    case 'profile':
        $err = Users::updateProfile($uid, trim((string)($_POST['display_name'] ?? '')), trim((string)($_POST['email'] ?? '')));
        flash($err ?? 'Profil gespeichert.', $err ? 'error' : 'info');
        redirect(webUrl(['view' => 'settings']));

    case 'password':
        $new = (string)($_POST['new'] ?? '');
        $err = Auth::confirmPassword((string)($_POST['current'] ?? ''));
        if ($err === null && $new !== (string)($_POST['new2'] ?? '')) {
            $err = 'Die neuen Passwörter stimmen nicht überein.';
        }
        $err = $err ?? Users::setPassword($uid, $new);
        $msg = 'Passwort geändert. Andere Browser-Sitzungen wurden abgemeldet.';
        if ($err === null) {
            Auth::startSession(Users::find($uid));
            if (!empty($_POST['revoke_tokens'])) {
                $n   = AppPasswords::revokeAll($uid);
                $msg .= $n > 0 ? " $n App-Passwort/-Passwörter widerrufen." : '';
            }
        }
        flash($err ?? $msg, $err ? 'error' : 'info');
        redirect(webUrl(['view' => 'settings']));

    case 'apppw_create':
        $name = trim((string)($_POST['name'] ?? ''));
        /* Mit Kontopasswort: eine gestohlene Sitzung soll sich keinen
         * dauerhaften API-Zugang verschaffen koennen. */
        $err = Auth::confirmPassword((string)($_POST['current'] ?? ''));
        if ($err !== null) {
            flash($err, 'error');
            redirect(webUrl(['view' => 'settings']) . '#devices');
        }
        [$token, $err] = AppPasswords::create($uid, $name);
        if ($err !== null) {
            flash($err, 'error');
        } else {
            $_SESSION['new_token'] = ['name' => $name, 'token' => $token];
        }
        redirect(webUrl(['view' => 'settings']) . '#devices');

    case 'apppw_revoke':
        AppPasswords::revoke($uid, (int)($_POST['id'] ?? 0));
        flash('App-Passwort widerrufen.');
        redirect(webUrl(['view' => 'settings']) . '#devices');

    case 'user_create':
        requireAdmin();
        [$id, $err] = Users::create(
            trim((string)($_POST['username'] ?? '')), (string)($_POST['password'] ?? ''),
            trim((string)($_POST['display_name'] ?? '')), trim((string)($_POST['email'] ?? '')),
            !empty($_POST['is_admin'])
        );
        flash($err ?? 'Benutzer ' . trim((string)$_POST['username']) . ' angelegt.', $err ? 'error' : 'info');
        redirect(webUrl(['view' => 'users']));

    case 'user_update':
        requireAdmin();
        $id  = (int)($_POST['id'] ?? 0);
        $err = Users::updateProfile($id, trim((string)($_POST['display_name'] ?? '')), trim((string)($_POST['email'] ?? '')));
        $err = $err ?? Users::updateAdmin($id, !empty($_POST['is_admin']), !empty($_POST['active']));
        if ($err === null && (string)($_POST['password'] ?? '') !== '') {
            $err = Users::setPassword($id, (string)$_POST['password']);
            if ($err === null && !empty($_POST['revoke_tokens'])) {
                AppPasswords::revokeAll($id);
            }
            if ($err === null && $id === $uid) {
                Auth::startSession(Users::find($uid));
            }
        }
        flash($err ?? 'Änderungen gespeichert.', $err ? 'error' : 'info');
        redirect(webUrl(['view' => 'users']));

    case 'user_delete':
        requireAdmin();
        $row = Users::find((int)($_POST['id'] ?? 0));
        if ($row === null || (string)($_POST['confirm'] ?? '') !== $row['username']) {
            flash('Löschen abgebrochen: Der eingetippte Name stimmt nicht.', 'error');
        } else {
            $err = Users::delete((int)$row['id'], $uid);
            flash($err ?? "Benutzer {$row['username']} gelöscht.", $err ? 'error' : 'info');
        }
        redirect(webUrl(['view' => 'users']));

    default:
        http_response_code(400);
        exit('Unbekannte Aktion.');
    }
}

/* ── Repository-Seiten (auch anonym, Zugriff per Repos::forView) ─────── */
if (isset($_GET['r'])) {
    $parts = explode('/', (string)$_GET['r'], 2);
    $repo  = count($parts) === 2 ? Repos::forView($parts[0], $parts[1]) : null;
    if ($repo === null) {
        notFound('Dieses Repository gibt es nicht – oder es ist privat.');
    }
    $rd   = new RepoReader($repo);
    $view = (string)($_GET['v'] ?? 'code');

    if ($view === 'branches') {
        renderLayout($repo['name'], $repo['is_owner'] ? 'repos' : 'explore', renderBranches($repo, $rd));
        exit;
    }
    if ($view === 'settings') {
        if (!$repo['is_owner']) {
            notFound();
        }
        renderLayout($repo['name'], 'repos', renderRepoSettings($repo, $rd));
        exit;
    }
    if ($view === 'commit') {
        $c = (string)($_GET['c'] ?? '');
        if (!RepoReader::isOid($c) || $rd->commit($c) === null) {
            notFound('Diesen Commit gibt es nicht.');
        }
        renderLayout(subjectLine($rd->commit($c)['message']) . ' · ' . $repo['name'],
                     $repo['is_owner'] ? 'repos' : 'explore', renderCommit($repo, $rd, $c));
        exit;
    }
    if ($view === 'history') {
        $ref = (string)($_GET['ref'] ?? '');
        renderLayout('Historie · ' . $repo['name'], $repo['is_owner'] ? 'repos' : 'explore',
                     renderHistory($repo, $rd, $ref, min(200, max(0, (int)($_GET['page'] ?? 0)))));
        exit;
    }

    /* code, blob, raw, zip: brauchen einen Stand */
    $def = $rd->defaultBranch();
    if ($def === null) {
        renderLayout($repo['name'], $repo['is_owner'] ? 'repos' : 'explore', repoHeader($repo, 'code') . emptyRepo($repo));
        exit;
    }
    $ref       = (string)($_GET['ref'] ?? $def['short']);
    $commitOid = $rd->resolve($ref);
    if ($commitOid === null) {
        notFound('Diesen Branch oder Commit gibt es nicht.');
    }
    $path = trim((string)($_GET['p'] ?? ''), '/');
    $node = $rd->lookup($commitOid, $path);
    if ($node === null) {
        notFound('Diesen Pfad gibt es in diesem Stand nicht.');
    }

    if ($view === 'zip') {
        if ($node['type'] !== 'tree') {
            notFound();
        }
        $label = RepoReader::isOid($ref) ? shortOid($ref) : preg_replace('/[^A-Za-z0-9_.-]/', '-', $ref);
        $root  = $repo['name'] . '-' . $label . ($path !== '' ? '-' . preg_replace('/[^A-Za-z0-9_.-]/', '-', basename($path)) : '');
        $tmp   = $rd->zip($node['oid'], $root);
        if ($tmp[0] !== '/') {
            flash($tmp, 'error');
            redirect(repoUrl($repo, 'code', ['ref' => $ref, 'p' => $path]));
        }
        @set_time_limit(0);
        sendFile($tmp, $root . '.zip', 'application/zip', false);
    }
    if ($view === 'raw') {
        if ($node['type'] !== 'blob' || ($file = $rd->blobPath($node['oid'])) === null) {
            notFound();
        }
        $image = imageMime($node['name']);
        if (!empty($_GET['dl'])) {
            sendFile($file, $node['name'], 'application/octet-stream', false);
        }
        if ($image !== null) {
            sendFile($file, $node['name'], $image, true);
        }
        $head = (string)file_get_contents($file, false, null, 0, 8000);
        if (RepoReader::isBinary($head)) {
            sendFile($file, $node['name'], 'application/octet-stream', false);
        }
        $all = filesize($file) <= DISPLAY_MAX_BYTES ? (string)file_get_contents($file) : $head;
        $cs  = mb_check_encoding($all, 'UTF-8') ? 'utf-8' : 'iso-8859-1';
        sendFile($file, $node['name'], 'text/plain; charset=' . $cs, true);
    }

    $active = $repo['is_owner'] ? 'repos' : 'explore';
    if ($node['type'] === 'blob') {
        renderLayout($node['name'] . ' · ' . $repo['name'], $active, renderBlob($repo, $rd, $ref, $commitOid, $path, $node));
    } else {
        renderLayout($repo['name'], $active, renderCode($repo, $rd, $ref, $commitOid, $path, $node));
    }
    exit;
}

/* ── Uebrige Ansichten ───────────────────────────────────────────────── */
$view = (string)($_GET['view'] ?? ($loggedIn ? 'repos' : 'explore'));
switch ($view) {
case 'login':
    if ($loggedIn) {
        redirect('/web.php');
    }
    renderLogin('');
    break;

case 'explore':
    $q = trim((string)($_GET['q'] ?? ''));
    renderLayout('Entdecken', 'explore', renderExplore($q), $q);
    break;

case 'settings':
    requireLogin();
    $newToken = $_SESSION['new_token'] ?? null;
    unset($_SESSION['new_token']);
    renderLayout('Einstellungen', 'settings', renderSettings($newToken));
    break;

case 'users':
    requireAdmin();
    renderLayout('Benutzer', 'users', renderUsers());
    break;

default:
    requireLogin();
    renderLayout('Meine Repositories', 'repos', renderMyRepos());
}
