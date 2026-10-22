<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Benutzer-Modell (Uebernahme aus AmiCloud, docs/design/webui-plan.md).
 * Der Benutzername ist unveraenderlich, weil er den Speicherort
 * AMIVCS_STORAGE_PATH/<name>/<repo> bildet und im Web in besitzer/repo
 * steht;
 * aenderbar ist der Anzeigename. Alle Methoden, die etwas schreiben,
 * liefern null bei Erfolg oder eine deutsche Fehlermeldung fuer die
 * Weboberflaeche bzw. das Admin-CLI. */
class Users
{
    const NAME_RE   = '/^[a-z0-9][a-z0-9._-]{0,63}$/';
    /* Muster der App-Passwoerter (AppPasswords::generate()). Auth erkennt
     * sie daran und prueft dann nur per SHA-256 statt bcrypt -- ein
     * Kontopasswort darf deshalb nicht so aussehen. */
    const APP_PW_RE = '/^[a-z2-9]{5}(-[a-z2-9]{5}){4}$/';
    /* Feste Kosten, damit Hashes von PHP 7.4 (Server) und 8.x (lokal)
     * gleich teuer sind -- Auth::DUMMY_HASH hat dieselben Kosten. */
    const BCRYPT_COST = 10;

    const COLS = 'id, username, pass_hash, created_at, active, display_name, email,
                  is_admin, last_login, session_epoch';

    public static function find(int $id): ?array
    {
        $stmt = DB::get()->prepare('SELECT ' . self::COLS . ' FROM users WHERE id = ?');
        $stmt->execute([$id]);
        $row = $stmt->fetch();
        return $row ?: null;
    }

    /* Collation ist case-insensitiv: "BOB" findet bob. Aufrufer muessen
     * danach mit $row['username'] weiterarbeiten, nie mit der Eingabe. */
    public static function byName(string $name): ?array
    {
        $stmt = DB::get()->prepare('SELECT ' . self::COLS . ' FROM users WHERE username = ? LIMIT 1');
        $stmt->execute([substr($name, 0, 64)]);
        $row = $stmt->fetch();
        return $row ?: null;
    }

    /* Alle Benutzer inkl. Anzahl Repos, fuer die Admin-Seite und user:list */
    public static function all(): array
    {
        return DB::get()->query(
            'SELECT u.id, u.username, u.created_at, u.active, u.display_name, u.email,
                    u.is_admin, u.last_login,
                    (SELECT COUNT(*) FROM repos r WHERE r.owner_id = u.id) AS repo_count
             FROM users u ORDER BY u.username'
        )->fetchAll();
    }

    public static function displayName(array $row): string
    {
        return $row['display_name'] !== '' ? $row['display_name'] : $row['username'];
    }

    /* ── Validierung ────────────────────────────────────────────────── */

    public static function validateName(string $name): ?string
    {
        if (!preg_match(self::NAME_RE, $name)) {
            return 'Benutzername: 1–64 Zeichen, nur a–z, 0–9, Punkt, Bindestrich und Unterstrich; '
                 . 'muss mit Buchstabe oder Ziffer beginnen.';
        }
        return null;
    }

    public static function validatePassword(string $pass): ?string
    {
        if (strlen($pass) < 8) {
            return 'Das Passwort muss mindestens 8 Zeichen lang sein.';
        }
        if (strlen($pass) > 200) {
            return 'Das Passwort darf höchstens 200 Zeichen lang sein.';
        }
        if (preg_match(self::APP_PW_RE, $pass)) {
            return 'Dieses Passwort hat das Format eines App-Passworts und ist als Kontopasswort nicht erlaubt.';
        }
        return null;
    }

    private static function validateProfile(string $display, string $email): ?string
    {
        if (mb_strlen($display, 'UTF-8') > 128) {
            return 'Der Anzeigename darf höchstens 128 Zeichen lang sein.';
        }
        if (preg_match('/[\x00-\x1f\x7f]/', $display)) {
            return 'Der Anzeigename enthält Steuerzeichen.';
        }
        if ($email !== '' && (strlen($email) > 255 || filter_var($email, FILTER_VALIDATE_EMAIL) === false)) {
            return 'Die E-Mail-Adresse ist ungültig.';
        }
        return null;
    }

    /* ── Schreiben ──────────────────────────────────────────────────── */

    private static function userDir(string $name): string
    {
        return rtrim(AMIVCS_STORAGE_PATH, '/') . '/' . $name;
    }

    /* Liefert [id, null] oder [0, Fehlermeldung]. Ein vorhandener, nicht
     * leerer Speicherordner gleichen Namens (Rest eines gescheiterten
     * Loeschens) blockiert das Anlegen, damit niemand fremde Repos erbt.
     * Die Repo-Ordner selbst legt erst create-repo an. */
    public static function create(
        string $name, string $pass, string $display = '', string $email = '', bool $admin = false
    ): array {
        $err = self::validateName($name) ?? self::validatePassword($pass)
            ?? self::validateProfile($display, $email);
        if ($err !== null) {
            return [0, $err];
        }
        if (self::byName($name) !== null) {
            return [0, "Den Benutzer $name gibt es schon."];
        }
        $dir = self::userDir($name);
        if (is_dir($dir) && count(scandir($dir)) > 2) {
            return [0, "Der Speicherordner $dir existiert bereits und ist nicht leer."];
        }

        DB::get()->prepare(
            'INSERT INTO users (username, pass_hash, display_name, email, is_admin)
             VALUES (?, ?, ?, ?, ?)'
        )->execute([
            $name,
            password_hash($pass, PASSWORD_BCRYPT, ['cost' => self::BCRYPT_COST]),
            $display, $email, $admin ? 1 : 0,
        ]);
        return [(int)DB::get()->lastInsertId(), null];
    }

    /* Wuerde die Aenderung den letzten aktiven Admin entfernen? */
    private static function wouldRemoveLastAdmin(array $row, bool $admin, bool $active): bool
    {
        if (!$row['is_admin'] || !$row['active'] || ($admin && $active)) {
            return false;
        }
        $n = (int)DB::get()->query('SELECT COUNT(*) FROM users WHERE is_admin = 1 AND active = 1')
                          ->fetchColumn();
        return $n <= 1;
    }

    public static function updateProfile(int $id, string $display, string $email): ?string
    {
        $err = self::validateProfile($display, $email);
        if ($err !== null) {
            return $err;
        }
        DB::get()->prepare('UPDATE users SET display_name = ?, email = ? WHERE id = ?')
                 ->execute([$display, $email, $id]);
        return null;
    }

    /* Admin-Felder. Deaktivieren erhoeht session_epoch, damit offene
     * Web-Sessions sofort enden; App-Passwoerter scheitern an active = 0. */
    public static function updateAdmin(int $id, bool $admin, bool $active): ?string
    {
        $row = self::find($id);
        if ($row === null) {
            return 'Benutzer nicht gefunden.';
        }
        if (self::wouldRemoveLastAdmin($row, $admin, $active)) {
            return 'Der letzte aktive Administrator kann weder herabgestuft noch deaktiviert werden.';
        }
        $bump = ($row['active'] && !$active) ? 1 : 0;
        DB::get()->prepare(
            'UPDATE users SET is_admin = ?, active = ?,
                              session_epoch = session_epoch + ? WHERE id = ?'
        )->execute([$admin ? 1 : 0, $active ? 1 : 0, $bump, $id]);
        return null;
    }

    /* Beendet alle Web-Sessions dieses Benutzers (session_epoch++).
     * App-Passwoerter bleiben gueltig (wie bei ownCloud). */
    public static function setPassword(int $id, string $pass): ?string
    {
        $err = self::validatePassword($pass);
        if ($err !== null) {
            return $err;
        }
        DB::get()->prepare(
            'UPDATE users SET pass_hash = ?, session_epoch = session_epoch + 1 WHERE id = ?'
        )->execute([password_hash($pass, PASSWORD_BCRYPT, ['cost' => self::BCRYPT_COST]), $id]);
        return null;
    }

    public static function touchLogin(int $id): void
    {
        DB::get()->prepare('UPDATE users SET last_login = NOW() WHERE id = ?')->execute([$id]);
    }

    /* Loescht DB-Zeilen (repos/objects/refs/app_passwords per CASCADE) und
     * den Speicherordner mit allen Repos. $actingId = wer loescht
     * (0 = Admin-CLI). */
    public static function delete(int $id, int $actingId = 0): ?string
    {
        $row = self::find($id);
        if ($row === null) {
            return 'Benutzer nicht gefunden.';
        }
        if ($id === $actingId) {
            return 'Das eigene Konto kann hier nicht gelöscht werden.';
        }
        if (self::wouldRemoveLastAdmin($row, false, false)) {
            return 'Der letzte aktive Administrator kann nicht gelöscht werden.';
        }

        /* Zielpfad doppelt absichern, bevor rekursiv geloescht wird: ein
         * leerer oder ungueltiger Name wuerde sonst AMIVCS_STORAGE_PATH selbst
         * treffen. */
        $name = (string)$row['username'];
        $dir  = self::userDir($name);
        if (preg_match(self::NAME_RE, $name) && is_dir($dir)) {
            $base = realpath(AMIVCS_STORAGE_PATH);
            if ($base === false || realpath(dirname($dir)) !== $base || realpath($dir) === $base) {
                return 'Speicherordner liegt nicht unter AMIVCS_STORAGE_PATH – Löschen abgebrochen.';
            }
            self::deleteTree($dir);
        }

        DB::get()->prepare('DELETE FROM users WHERE id = ?')->execute([$id]);
        return null;
    }

    private static function deleteTree(string $dir): void
    {
        foreach (scandir($dir) as $item) {
            if ($item === '.' || $item === '..') {
                continue;
            }
            $fp = $dir . '/' . $item;
            if (is_dir($fp) && !is_link($fp)) {
                self::deleteTree($fp);
            } else {
                unlink($fp);
            }
        }
        rmdir($dir);
    }
}
