<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
/* AmiVCS-Verwaltung auf der Kommandozeile (wie amicloud-admin.php bei
 * AmiCloud). Als Benutzer des Webservers aufrufen, damit geloeschte
 * Speicherordner die richtigen Rechte haben:
 *
 *   sudo -u apache php bin/amivcs-admin.php user:add --admin ronny
 *
 * Der Ordner bin/ ist per .htaccess fuer den Browser gesperrt. */
if (PHP_SAPI !== 'cli') {
    http_response_code(403);
    exit;
}
define('AMIVCS_SERVER', 1);

$root = dirname(__DIR__);
if (!is_readable($root . '/config.php')) {
    fwrite(STDERR, "config.php nicht lesbar – als Webserver-Benutzer aufrufen (sudo -u apache …).\n");
    exit(1);
}
require $root . '/config.php';
require $root . '/includes/Version.php';
require $root . '/includes/Response.php';
require $root . '/includes/DB.php';
require $root . '/includes/Throttle.php';
require $root . '/includes/Users.php';
require $root . '/includes/AppPasswords.php';
require $root . '/includes/Auth.php';
require $root . '/includes/Repos.php';

function usage(): void
{
    echo <<<TXT
AmiVCS v{$GLOBALS['ver']} – Verwaltung

  user:list
  user:add [--admin] [--display=NAME] [--email=ADR] [--password-stdin] <name>
  user:passwd [--password-stdin] [--keep-tokens] <name>
                                           widerruft auch die App-Passwörter
  user:revoke-tokens <name>                alle App-Passwörter widerrufen
  user:admin <name> on|off
  user:disable <name>
  user:enable <name>
  user:delete [--yes] <name>               löscht auch alle Repositories
  repo:list [<benutzer>]

TXT;
}

function fail(string $msg): void
{
    fwrite(STDERR, $msg . "\n");
    exit(1);
}

/* Passwort ohne Echo lesen (bzw. eine Zeile von stdin) */
function readPassword(bool $fromStdin, string $prompt): string
{
    if ($fromStdin) {
        return rtrim((string)fgets(STDIN), "\r\n");
    }
    fwrite(STDOUT, $prompt);
    $tty = function_exists('posix_isatty') ? posix_isatty(STDIN) : true;
    if ($tty) {
        shell_exec('stty -echo');
    }
    $p = rtrim((string)fgets(STDIN), "\r\n");
    if ($tty) {
        shell_exec('stty echo');
        fwrite(STDOUT, "\n");
    }
    return $p;
}

function askNewPassword(bool $fromStdin): string
{
    $p = readPassword($fromStdin, 'Passwort: ');
    if (!$fromStdin && readPassword(false, 'Passwort wiederholen: ') !== $p) {
        fail('Die Passwörter stimmen nicht überein.');
    }
    return $p;
}

function userOrFail(string $name): array
{
    $row = Users::byName($name);
    if ($row === null) {
        fail("Benutzer $name nicht gefunden.");
    }
    return $row;
}

$ver  = AMIVCS_VERSION;
$args = array_slice($argv, 1);
$cmd  = array_shift($args) ?? '';
$opts = [];
$pos  = [];
foreach ($args as $a) {
    if (preg_match('/^--([a-z-]+)(?:=(.*))?$/', $a, $m)) {
        $opts[$m[1]] = $m[2] ?? true;
    } else {
        $pos[] = $a;
    }
}
$stdinPw = isset($opts['password-stdin']);

switch ($cmd) {
case 'user:list':
    printf("%-20s %-24s %-6s %-6s %-6s %s\n", 'BENUTZER', 'ANZEIGENAME', 'ADMIN', 'AKTIV', 'REPOS', 'LETZTER LOGIN');
    foreach (Users::all() as $u) {
        printf("%-20s %-24s %-6s %-6s %-6d %s\n",
            $u['username'], mb_strimwidth($u['display_name'], 0, 24, '…', 'UTF-8'),
            $u['is_admin'] ? 'ja' : '-', $u['active'] ? 'ja' : 'nein',
            (int)$u['repo_count'], $u['last_login'] ?? '-');
    }
    break;

case 'repo:list':
    $owner = $pos[0] ?? null;
    printf("%-20s %-30s %-11s %-9s %s\n", 'BESITZER', 'REPOSITORY', 'SICHTBAR', 'BRANCHES', 'LETZTER PUSH');
    foreach (Users::all() as $u) {
        if ($owner !== null && strcasecmp($owner, $u['username']) !== 0) {
            continue;
        }
        foreach (Repos::listOwn((int)$u['id']) as $r) {
            printf("%-20s %-30s %-11s %-9d %s\n", $u['username'], $r['name'],
                (int)$r['is_public'] ? 'öffentlich' : 'privat', (int)$r['branch_count'], $r['pushed_at'] ?? '-');
        }
    }
    break;

case 'user:add':
    if (count($pos) !== 1) { usage(); exit(1); }
    [$id, $err] = Users::create($pos[0], askNewPassword($stdinPw),
        (string)($opts['display'] ?? ''), (string)($opts['email'] ?? ''), isset($opts['admin']));
    if ($err !== null) fail($err);
    echo "Benutzer {$pos[0]} angelegt" . (isset($opts['admin']) ? ' (Administrator)' : '') . ".\n";
    break;

case 'user:passwd':
    if (count($pos) !== 1) { usage(); exit(1); }
    $u = userOrFail($pos[0]);
    $err = Users::setPassword((int)$u['id'], askNewPassword($stdinPw));
    if ($err !== null) fail($err);
    $n = isset($opts['keep-tokens']) ? 0 : AppPasswords::revokeAll((int)$u['id']);
    echo "Passwort von {$u['username']} geändert; offene Web-Sitzungen sind beendet"
       . ($n > 0 ? ", $n App-Passwort/-Passwörter widerrufen" : '') . ".\n";
    break;

case 'user:revoke-tokens':
    if (count($pos) !== 1) { usage(); exit(1); }
    $u = userOrFail($pos[0]);
    echo AppPasswords::revokeAll((int)$u['id']) . " App-Passwort/-Passwörter von {$u['username']} widerrufen.\n";
    break;

case 'user:admin':
case 'user:disable':
case 'user:enable':
    $need = $cmd === 'user:admin' ? 2 : 1;
    if (count($pos) !== $need) { usage(); exit(1); }
    $u      = userOrFail($pos[0]);
    $admin  = (bool)(int)$u['is_admin'];
    $active = (bool)(int)$u['active'];
    if ($cmd === 'user:admin') {
        if (!in_array($pos[1], ['on', 'off'], true)) fail('Erwartet: on oder off');
        $admin = $pos[1] === 'on';
    } else {
        $active = $cmd === 'user:enable';
    }
    $err = Users::updateAdmin((int)$u['id'], $admin, $active);
    if ($err !== null) fail($err);
    echo "Benutzer {$u['username']} geändert.\n";
    break;

case 'user:delete':
    if (count($pos) !== 1) { usage(); exit(1); }
    $u = userOrFail($pos[0]);
    if (!isset($opts['yes'])) {
        fwrite(STDOUT, "Benutzer {$u['username']} und ALLE seine Repositories löschen? Zur Bestätigung den Namen eintippen: ");
        if (trim((string)fgets(STDIN)) !== $u['username']) fail('Abgebrochen.');
    }
    $err = Users::delete((int)$u['id']);
    if ($err !== null) fail($err);
    echo "Benutzer {$u['username']} gelöscht.\n";
    break;

default:
    usage();
    exit($cmd === '' || $cmd === 'help' ? 0 : 1);
}
