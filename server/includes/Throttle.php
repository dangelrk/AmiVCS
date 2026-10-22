<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Brute-Force-Bremse fuer Web-Login, Basic-Auth und Link-Passwoerter.
 * Zwei Grenzen im 15-Minuten-Fenster: pro (IP, Bereich) -- Bereich ist
 * der Benutzername oder "share:<token>" -- und hoeher pro IP, damit ein
 * falsch konfigurierter Amiga nach einer Passwortaenderung nicht gleich
 * den ganzen Haushalt hinter demselben Router aussperrt. Geschrieben
 * wird nur im Fehlerfall. */
class Throttle
{
    const PER_SCOPE = 5;
    const PER_IP    = 30;

    public static function ip(): string
    {
        return substr((string)($_SERVER['REMOTE_ADDR'] ?? ''), 0, 45);
    }

    /* [Fehlversuche im Bereich, Fehlversuche der IP] */
    public static function failures(string $scope): array
    {
        $stmt = DB::get()->prepare(
            'SELECT COALESCE(SUM(scope = ?), 0) AS s, COUNT(*) AS i
               FROM auth_failures WHERE ip = ? AND at > NOW() - INTERVAL 15 MINUTE'
        );
        $stmt->execute([substr($scope, 0, 96), self::ip()]);
        $row = $stmt->fetch();
        return [(int)$row['s'], (int)$row['i']];
    }

    public static function blocked(array $failures, int $perScope = self::PER_SCOPE): bool
    {
        return $failures[0] >= $perScope || $failures[1] >= self::PER_IP;
    }

    public static function fail(string $scope): void
    {
        $db = DB::get();
        $db->exec('DELETE FROM auth_failures WHERE at < NOW() - INTERVAL 1 DAY');
        $db->prepare('INSERT INTO auth_failures (ip, scope) VALUES (?, ?)')
           ->execute([self::ip(), substr($scope, 0, 96)]);
    }

    public static function clear(string $scope): void
    {
        DB::get()->prepare('DELETE FROM auth_failures WHERE ip = ? AND scope = ?')
                 ->execute([self::ip(), substr($scope, 0, 96)]);
    }
}
