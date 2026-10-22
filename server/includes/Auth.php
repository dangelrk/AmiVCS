<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Eigene Benutzertabelle, getrennt von AmiCloud: ein AMIVCS-Konto hat
 * nichts mit einem AmiCloud-Konto zu tun, auch wenn dieselbe Person
 * beide hat. Uebernommen aus AmiCloud (App-Passwoerter, Login-Bremse,
 * Session-Epoche), docs/design/webui-plan.md. */
class Auth
{
    public static int    $user_id  = 0;
    public static string $username = '';
    public static bool   $is_admin = false;
    public static ?array $user     = null;

    /* bcrypt mit denselben Kosten wie Users::BCRYPT_COST; wird bei
     * unbekanntem Benutzer geprueft, damit die Antwortzeit nicht verraet,
     * ob es den Namen gibt. */
    const DUMMY_HASH = '$2y$10$juFV3MtL9EZJJ/MebYw4kOPtPB8pbD3PEkVK5rU4Mq/9fikZ10n02';

    /* Benutzername, Speicherpfad und Rechte kommen immer aus der DB-Zeile,
     * nie aus der Eingabe: die Collation ist case-insensitiv, "BOB" wuerde
     * sonst bobs Zeile, aber ein anderes Speicherverzeichnis treffen. 0 =
     * anonymer Besucher (nur oeffentliche Repos, web.php/index.php). */
    public static function setUser(array $row): void
    {
        self::$user_id  = (int)$row['id'];
        self::$username = (string)$row['username'];
        self::$is_admin = (bool)(int)$row['is_admin'];
        self::$user     = $row;
    }

    /* Kontopasswort pruefen; null = unbekannt oder falsch. */
    private static function checkPassword(string $name, string $pass): ?array
    {
        $row = Users::byName($name);
        $ok  = password_verify($pass, $row ? $row['pass_hash'] : self::DUMMY_HASH);
        return ($ok && $row && (int)$row['active']) ? $row : null;
    }

    /* API: Basic-Auth mit Kontopasswort ODER App-Passwort. App-Passwoerter
     * werden am Format erkannt und nur per SHA-256 geprueft -- sonst liefe
     * vorher bei jedem Amiga-Request ein bcrypt mit. */
    public static function requireBasicAuth(): void
    {
        $user = substr((string)($_SERVER['PHP_AUTH_USER'] ?? ''), 0, 64);
        $pass = (string)($_SERVER['PHP_AUTH_PW'] ?? '');

        if ($user === '' || $pass === '') {
            header('WWW-Authenticate: Basic realm="AMIVCS"');
            Response::error(401, 'Authentication required');
        }

        Throttle::lock($user);
        $fails = Throttle::failures($user);
        if (Throttle::blocked($fails)) {
            header('Retry-After: 900');
            Response::error(429, 'Too many failed logins');
        }

        if (preg_match(Users::APP_PW_RE, $pass)) {
            $row = Users::byName($user);
            if (!$row || !(int)$row['active'] || !AppPasswords::verify((int)$row['id'], $pass)) {
                $row = null;
            }
        } else {
            $row = self::checkPassword($user, $pass);
        }

        if ($row === null) {
            Throttle::fail($user);
            header('WWW-Authenticate: Basic realm="AMIVCS"');
            Response::error(401, 'Invalid credentials');
        }
        if ($fails[0] > 0) {
            Throttle::clear($user);
        }
        Throttle::unlock($user);
        self::setUser($row);
    }

    /* Web-Login: nur das Kontopasswort, keine App-Passwoerter. Liefert die
     * Benutzerzeile oder eine Fehlermeldung. */
    public static function webLogin(string $user, string $pass)
    {
        $user  = substr(trim($user), 0, 64);
        Throttle::lock($user);
        try {
            $fails = Throttle::failures($user);
            if (Throttle::blocked($fails)) {
                return 'Zu viele Fehlversuche. Bitte in 15 Minuten erneut versuchen.';
            }
            $row = ($user === '' || $pass === '') ? null : self::checkPassword($user, $pass);
            if ($row === null) {
                Throttle::fail($user);
                return 'Ungültige Zugangsdaten.';
            }
            if ($fails[0] > 0) {
                Throttle::clear($user);
            }
        } finally {
            Throttle::unlock($user);
        }
        Users::touchLogin((int)$row['id']);
        return $row;
    }

    /* Aktuelles Kontopasswort des angemeldeten Benutzers pruefen (vor
     * Passwortwechsel und neuem App-Passwort), mit derselben Bremse wie
     * der Login -- eine gestohlene Sitzung soll es nicht durchprobieren
     * koennen. null = ok, sonst Fehlermeldung. */
    public static function confirmPassword(string $pass): ?string
    {
        $user = self::$username;
        Throttle::lock($user);
        try {
            $fails = Throttle::failures($user);
            if (Throttle::blocked($fails)) {
                return 'Zu viele Fehlversuche. Bitte in 15 Minuten erneut versuchen.';
            }
            if (!password_verify($pass, (string)self::$user['pass_hash'])) {
                Throttle::fail($user);
                return 'Das aktuelle Passwort ist falsch.';
            }
            if ($fails[0] > 0) {
                Throttle::clear($user);
            }
        } finally {
            Throttle::unlock($user);
        }
        return null;
    }

    /* Web-Session -> Benutzer, bei JEDEM Request aus der DB: geloeschte,
     * deaktivierte Benutzer und Sessions von vor einem Passwortwechsel
     * (session_epoch) sind sofort ungueltig; Admin-Recht nie aus der
     * Session. */
    public static function loadSessionUser(): bool
    {
        if (!isset($_SESSION['user_id'])) {
            return false;
        }
        $row = Users::find((int)$_SESSION['user_id']);
        if (!$row || !(int)$row['active']
            || (int)$row['session_epoch'] !== (int)($_SESSION['epoch'] ?? -1)) {
            $_SESSION = [];
            session_regenerate_id(true);
            return false;
        }
        self::setUser($row);
        return true;
    }

    public static function startSession(array $row): void
    {
        session_regenerate_id(true);
        $_SESSION['user_id'] = (int)$row['id'];
        $_SESSION['epoch']   = (int)$row['session_epoch'];
        $_SESSION['csrf']    = bin2hex(random_bytes(16));
    }
}
