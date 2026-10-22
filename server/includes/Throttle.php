<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Brute-Force-Bremse fuer Web-Login, Basic-Auth, Passwortwechsel und
 * Link-Passwoerter. Drei Grenzen im 15-Minuten-Fenster:
 *   - pro (IP, Bereich)       -- Bereich = Benutzername bzw. "share:<token>"
 *   - pro IP                  -- hoeher, damit ein falsch konfigurierter
 *                                Amiga nicht den ganzen Haushalt aussperrt
 *   - pro Bereich ueber alle IPs -- gegen verteilte Angriffe
 * IPv6-Adressen zaehlen je /64 (sonst rotiert ein Angreifer einfach).
 * Bereiche werden klein geschrieben verglichen ("BOB" = "bob").
 *
 * Gegen parallele Requests, die alle "0 Fehlversuche" sehen wuerden,
 * laeuft Pruefen+Passwortvergleich+Protokollieren je Bereich unter einer
 * MariaDB-Sperre (lock()/unlock()). Geschrieben wird nur im Fehlerfall. */
class Throttle
{
    const PER_SCOPE   = 5;
    const PER_IP      = 30;
    const PER_ACCOUNT = 50;

    public static function ip(): string
    {
        $ip  = (string)($_SERVER['REMOTE_ADDR'] ?? '');
        $bin = @inet_pton($ip);
        if ($bin !== false && strlen($bin) === 16) {
            return inet_ntop(substr($bin, 0, 8) . str_repeat("\0", 8)) . '/64';
        }
        return substr($ip, 0, 45);
    }

    private static function norm(string $scope): string
    {
        return substr(mb_strtolower(trim($scope), 'UTF-8'), 0, 96);
    }

    private static function lockName(string $scope): string
    {
        return 'amivcsauth:' . md5(self::norm($scope));
    }

    /* Serialisiert alle Pruefungen fuer diesen Bereich; wird beim Ende
     * des Requests (Verbindungsende) ohnehin freigegeben. */
    public static function lock(string $scope): void
    {
        $stmt = DB::get()->prepare('SELECT GET_LOCK(?, 10)');
        $stmt->execute([self::lockName($scope)]);
    }

    public static function unlock(string $scope): void
    {
        $stmt = DB::get()->prepare('SELECT RELEASE_LOCK(?)');
        $stmt->execute([self::lockName($scope)]);
    }

    /* [Fehlversuche (IP, Bereich), Fehlversuche der IP, Fehlversuche des Bereichs] */
    public static function failures(string $scope): array
    {
        $stmt = DB::get()->prepare(
            'SELECT COALESCE(SUM(ip = ? AND scope = ?), 0) AS s,
                    COALESCE(SUM(ip = ?), 0) AS i,
                    COALESCE(SUM(scope = ?), 0) AS a
               FROM auth_failures
              WHERE (ip = ? OR scope = ?) AND at > NOW() - INTERVAL 15 MINUTE'
        );
        $ip = self::ip();
        $sc = self::norm($scope);
        $stmt->execute([$ip, $sc, $ip, $sc, $ip, $sc]);
        $row = $stmt->fetch();
        return [(int)$row['s'], (int)$row['i'], (int)$row['a']];
    }

    public static function blocked(array $failures, int $perScope = self::PER_SCOPE): bool
    {
        return $failures[0] >= $perScope || $failures[1] >= self::PER_IP
            || $failures[2] >= self::PER_ACCOUNT;
    }

    public static function fail(string $scope): void
    {
        $db = DB::get();
        if (random_int(0, 19) === 0) {
            $db->exec('DELETE FROM auth_failures WHERE at < NOW() - INTERVAL 1 DAY');
        }
        $db->prepare('INSERT INTO auth_failures (ip, scope) VALUES (?, ?)')
           ->execute([self::ip(), self::norm($scope)]);
    }

    public static function clear(string $scope): void
    {
        DB::get()->prepare('DELETE FROM auth_failures WHERE ip = ? AND scope = ?')
                 ->execute([self::ip(), self::norm($scope)]);
    }
}
