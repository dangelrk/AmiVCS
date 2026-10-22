<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Gemeinsame Helfer der Weboberflaeche (web.php, index.php); Grundstock
 * aus AmiCloud uebernommen, docs/design/webui-plan.md. */

function h(string $s): string
{
    /* Namen, Autoren und Nachrichten vom Amiga sind oft ISO-8859-1 */
    if (!mb_check_encoding($s, 'UTF-8')) {
        $s = mb_convert_encoding($s, 'UTF-8', 'ISO-8859-1');
    }
    return htmlspecialchars($s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function fmtSize(int $bytes): string
{
    if ($bytes < 1024)       return $bytes . ' B';
    if ($bytes < 1048576)    return str_replace('.', ',', (string)round($bytes / 1024, 1)) . ' KB';
    if ($bytes < 1073741824) return str_replace('.', ',', (string)round($bytes / 1048576, 1)) . ' MB';
    return str_replace('.', ',', (string)round($bytes / 1073741824, 1)) . ' GB';
}

/* "vor 5 Minuten", "gestern", sonst Datum -- wie ownCloud */
function relTime(int $ts): string
{
    $d = time() - $ts;
    if ($d < 60)    return 'gerade eben';
    if ($d < 3600)  return 'vor ' . (int)($d / 60) . ' Min.';
    if ($d < 86400 && date('Y-m-d', $ts) === date('Y-m-d')) return 'vor ' . (int)($d / 3600) . ' Std.';
    if (date('Y-m-d', $ts) === date('Y-m-d', time() - 86400)) return 'gestern';
    return date('d.m.Y', $ts);
}

function webUrl(array $q = []): string
{
    return '/web.php' . ($q ? '?' . http_build_query($q) : '');
}

function csrfField(): string
{
    return '<input type="hidden" name="csrf" value="' . h($_SESSION['csrf'] ?? '') . '">';
}

/* Meldung fuer die naechste Seite (Post/Redirect/Get) */
function flash(string $msg, string $type = 'info'): void
{
    $_SESSION['flash'][] = ['msg' => $msg, 'type' => $type];
}

function takeFlashes(): array
{
    $f = $_SESSION['flash'] ?? [];
    unset($_SESSION['flash']);
    return $f;
}

function redirect(string $url): void
{
    header('Location: ' . $url);
    exit;
}

function parentPath(string $path): string
{
    $p = dirname($path);
    return ($p === '.' || $p === '' || $p === '\\') ? '/' : $p;
}

function joinPath(string $dir, string $name): string
{
    return rtrim($dir, '/') . '/' . $name;
}

function icon(string $name, string $cls = 'ic'): string
{
    return '<svg class="' . $cls . '" aria-hidden="true"><use href="#i-' . $name . '"></use></svg>';
}

/* Dateityp -> Icon, inkl. Amiga-Formaten */
function fileKind(string $name, bool $isDir): string
{
    if ($isDir) {
        return 'folder';
    }
    $ext = strtolower(pathinfo($name, PATHINFO_EXTENSION));
    $map = [
        'image'   => ['png', 'jpg', 'jpeg', 'gif', 'bmp', 'iff', 'ilbm', 'svg', 'webp', 'tif', 'tiff'],
        'text'    => ['txt', 'md', 'guide', 'doc', 'asc', 'nfo', 'readme', 'pdf', 'html', 'htm', 'rtf'],
        'archive' => ['lha', 'lzh', 'lzx', 'zip', 'tar', 'gz', 'tgz', 'bz2', 'xz', '7z', 'rar'],
        'disk'    => ['adf', 'adz', 'dms', 'hdf', 'iso', 'img'],
        'audio'   => ['mod', 'med', 'wav', 'mp3', 'ogg', 'flac', 'aiff', '8svx', 'sid', 'xm', 's3m'],
        'code'    => ['c', 'h', 's', 'asm', 'i', 'rexx', 'py', 'php', 'js', 'cpp', 'hpp', 'sh', 'json', 'xml'],
    ];
    foreach ($map as $kind => $exts) {
        if (in_array($ext, $exts, true)) {
            return $kind;
        }
    }
    return 'file';
}

function initials(string $name): string
{
    $parts = preg_split('/[\s._-]+/u', trim($name), -1, PREG_SPLIT_NO_EMPTY);
    $s = '';
    foreach (array_slice($parts, 0, 2) as $p) {
        $s .= mb_strtoupper(mb_substr($p, 0, 1, 'UTF-8'), 'UTF-8');
    }
    return $s === '' ? '?' : $s;
}

function cssUrl(): string
{
    return '/web.css?v=' . filemtime(dirname(__DIR__, 2) . '/web.css');
}

function jsUrl(): string
{
    return '/web.js?v=' . filemtime(dirname(__DIR__, 2) . '/web.js');
}

function serverUrl(): string
{
    return 'https://' . ($_SERVER['HTTP_HOST'] ?? 'amivcs.example.com');
}

/* Link auf eine Repo-Seite: v = code|blob|raw|history|commit|branches|settings|zip */
function repoUrl(array $repo, string $view = 'code', array $q = []): string
{
    return webUrl(array_merge(['r' => $repo['owner'] . '/' . $repo['name'], 'v' => $view], $q));
}

function shortOid(string $oid): string
{
    return substr($oid, 0, 8);
}

/* Erste Zeile einer Commit-Nachricht */
function subjectLine(string $msg): string
{
    $line = strtok($msg, "\n");
    return $line === false ? '' : rtrim($line);
}

/* SVG-Sprite: einmal pro Seite ausgeben, dann icon('name') */
function iconSprite(): string
{
    $paths = [
        'folder'   => '<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5h9A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z" class="fill"/>',
        'file'     => '<path d="M6 3h8l5 5v12a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M14 3v5h5"/>',
        'text'     => '<path d="M6 3h8l5 5v12a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M14 3v5h5M8.5 12.5h7M8.5 16h7"/>',
        'image'    => '<rect x="3.5" y="4.5" width="17" height="15" rx="1.5"/><circle cx="9" cy="10" r="1.6"/><path d="m4 18 5.5-5.5 4 4 2.5-2.5 4.5 4.5"/>',
        'archive'  => '<rect x="4" y="3.5" width="16" height="17" rx="1.5"/><path d="M12 3.5v2m0 2v2m0 2v2"/><rect x="10.5" y="13.5" width="3" height="3.5" rx=".8"/>',
        'disk'     => '<path d="M4 4.5A1 1 0 0 1 5 3.5h12.5l2.5 2.5v13.5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1z"/><path d="M8 3.5V9h8V3.5M13.5 5v2.5"/><rect x="7" y="13" width="10" height="7.5" rx=".5"/>',
        'audio'    => '<path d="M9 18V5.5l11-2V16"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="17.5" cy="16" r="2.5"/>',
        'code'     => '<path d="m8.5 8-4 4 4 4M15.5 8l4 4-4 4M13.5 5.5l-3 13"/>',
        'search'   => '<circle cx="11" cy="11" r="6.5"/><path d="m16 16 4.5 4.5"/>',
        'menu'     => '<path d="M4 7h16M4 12h16M4 17h16"/>',
        'plus'     => '<path d="M12 5v14M5 12h14"/>',
        'upload'   => '<path d="M12 15V4m-4.5 4.5L12 4l4.5 4.5M4.5 15v4.5h15V15"/>',
        'download' => '<path d="M12 4v11m-4.5-4.5L12 15l4.5-4.5M4.5 15v4.5h15V15"/>',
        'trash'    => '<path d="M4.5 7h15M9.5 7V4.5h5V7M6.5 7l1 13h9l1-13M10 11v5.5M14 11v5.5"/>',
        'edit'     => '<path d="M14.5 5.5 18.5 9.5 9 19H5v-4z"/><path d="m13 7 4 4"/>',
        'move'     => '<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5h9A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/><path d="M9 13.5h6m-2.5-2.5 2.5 2.5-2.5 2.5"/>',
        'share'    => '<path d="M10 14a4 4 0 0 0 5.66 0l3-3a4 4 0 0 0-5.66-5.66l-1 1"/><path d="M14 10a4 4 0 0 0-5.66 0l-3 3a4 4 0 0 0 5.66 5.66l1-1"/>',
        'info'     => '<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5.5M12 7.5v.5"/>',
        'more'     => '<circle cx="5.5" cy="12" r="1.4" class="fill"/><circle cx="12" cy="12" r="1.4" class="fill"/><circle cx="18.5" cy="12" r="1.4" class="fill"/>',
        'gear'     => '<circle cx="12" cy="12" r="3"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"/>',
        'users'    => '<circle cx="9" cy="8.5" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0M16 5.2a3.5 3.5 0 0 1 0 6.6M18 14.5a6.5 6.5 0 0 1 3.5 5.5"/>',
        'clock'    => '<circle cx="12" cy="12" r="8.5"/><path d="M12 7v5l3.5 2"/>',
        'history'  => '<path d="M4 12a8 8 0 1 0 2.4-5.7M4 4.5V8h3.5"/><path d="M12 8v4.5l3 1.8"/>',
        'home'     => '<path d="m3.5 11 8.5-7 8.5 7M6 9v11h4.5v-6h3v6H18V9"/>',
        'logout'   => '<path d="M14 4.5h4.5a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1H14M10 16l-4-4 4-4M6 12h10"/>',
        'close'    => '<path d="m6 6 12 12M18 6 6 18"/>',
        'key'      => '<circle cx="8" cy="15" r="4"/><path d="m11 12 8.5-8.5M16.5 6.5l2 2M14.5 8.5l1.5 1.5"/>',
        'lock'     => '<rect x="5" y="10.5" width="14" height="10" rx="1.5"/><path d="M8 10.5V8a4 4 0 0 1 8 0v2.5"/>',
        'copy'     => '<rect x="8.5" y="8.5" width="11" height="11" rx="1.5"/><path d="M15.5 8.5V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v9.5a1 1 0 0 0 1 1h3.5"/>',
        'chevron'  => '<path d="m9 6 6 6-6 6"/>',
        'repo'     => '<path d="M6 3.5h11.5a1 1 0 0 1 1 1V17H6.5a2 2 0 0 0-2 2V5a1.5 1.5 0 0 1 1.5-1.5z"/><path d="M4.5 19a2 2 0 0 0 2 2h12v-4M9 3.5v7l2-1.5 2 1.5v-7"/>',
        'branch'   => '<circle cx="7" cy="5.5" r="2.2"/><circle cx="7" cy="18.5" r="2.2"/><circle cx="17" cy="7.5" r="2.2"/><path d="M7 7.7v8.6M17 9.7c0 4-10 2.5-10 6.6"/>',
        'commit'   => '<circle cx="12" cy="12" r="3.5"/><path d="M3 12h5.5M15.5 12H21"/>',
        'globe'    => '<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17M12 3.5c2.5 2.6 3.5 5.4 3.5 8.5s-1 5.9-3.5 8.5c-2.5-2.6-3.5-5.4-3.5-8.5s1-5.9 3.5-8.5z"/>',
        'login'    => '<path d="M10 4.5H5.5a1 1 0 0 0-1 1v13a1 1 0 0 0 1 1H10M14 16l4-4-4-4M18 12H8"/>',
    ];
    $out = '<svg xmlns="http://www.w3.org/2000/svg" style="display:none">';
    foreach ($paths as $id => $p) {
        $out .= '<symbol id="i-' . $id . '" viewBox="0 0 24 24">' . $p . '</symbol>';
    }
    return $out . '</svg>';
}
