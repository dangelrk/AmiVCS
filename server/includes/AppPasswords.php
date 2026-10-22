<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Geraete-/App-Passwoerter: zufaellige Tokens, die statt des
 * Kontopassworts in amivcs.cfg stehen und einzeln widerrufbar sind.
 * Gespeichert als SHA-256, nicht bcrypt: bei ~124 bit Entropie bringt
 * ein langsamer Hash nichts, kostet aber bei jedem Amiga-Request Zeit. */
class AppPasswords
{
    /* Ohne l/o/0/1 -- vom Bildschirm abgetippt wird sonst verwechselt */
    const ALPHABET = 'abcdefghijkmnpqrstuvwxyz23456789';

    private static function generate(): string
    {
        $max    = strlen(self::ALPHABET) - 1;
        $groups = [];
        for ($g = 0; $g < 5; $g++) {
            $s = '';
            for ($i = 0; $i < 5; $i++) {
                $s .= self::ALPHABET[random_int(0, $max)];
            }
            $groups[] = $s;
        }
        return implode('-', $groups);
    }

    /* Liefert [Klartext-Token, null] oder ['', Fehlermeldung]. Das Token
     * wird nur dieses eine Mal angezeigt. */
    public static function create(int $uid, string $name): array
    {
        $name = trim($name);
        if ($name === '' || mb_strlen($name, 'UTF-8') > 64 || preg_match('/[\x00-\x1f\x7f]/', $name)) {
            return ['', 'Bitte einen Gerätenamen mit 1–64 Zeichen angeben.'];
        }
        $token = self::generate();
        DB::get()->prepare('INSERT INTO app_passwords (user_id, name, token_hash) VALUES (?, ?, ?)')
                 ->execute([$uid, $name, hash('sha256', $token)]);
        return [$token, null];
    }

    public static function listFor(int $uid): array
    {
        $stmt = DB::get()->prepare(
            'SELECT id, name, created_at, last_used_at FROM app_passwords
              WHERE user_id = ? ORDER BY created_at DESC'
        );
        $stmt->execute([$uid]);
        return $stmt->fetchAll();
    }

    public static function revoke(int $uid, int $id): void
    {
        DB::get()->prepare('DELETE FROM app_passwords WHERE id = ? AND user_id = ?')->execute([$id, $uid]);
    }

    /* last_used_at nur alle 5 Minuten schreiben -- sonst kostet jeder
     * einzelne Amiga-Request einen DB-Schreibzugriff. */
    public static function verify(int $uid, string $token): bool
    {
        $stmt = DB::get()->prepare(
            'SELECT id, (last_used_at IS NULL OR last_used_at < NOW() - INTERVAL 5 MINUTE) AS stale
               FROM app_passwords WHERE token_hash = ? AND user_id = ?'
        );
        $stmt->execute([hash('sha256', $token), $uid]);
        $row = $stmt->fetch();
        if (!$row) {
            return false;
        }
        if ((int)$row['stale'] === 1) {
            DB::get()->prepare('UPDATE app_passwords SET last_used_at = NOW() WHERE id = ?')
                     ->execute([$row['id']]);
        }
        return true;
    }
}
