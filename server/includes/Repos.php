<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Repo name -> storage path / DB row resolution, scoped to the
 * authenticated user -- v1 has no shared/team repositories (docs/adr/
 * 09_ADR_VCS_Design.md's v1 scope doesn't call for it), so a repo name
 * is only unique per owner, not server-wide. */
class Repos
{
    private static function sanitizeName(string $name): string
    {
        if (!preg_match('/^[A-Za-z0-9_.-]{1,128}$/', $name)) {
            Response::error(400, 'Invalid repo name');
        }
        return $name;
    }

    public static function storagePath(string $name): string
    {
        return rtrim(AMIVCS_STORAGE_PATH, '/') . '/' . Auth::$username . '/' . self::sanitizeName($name);
    }

    public static function create(string $name): void
    {
        $name = self::sanitizeName($name);

        $stmt = DB::get()->prepare('SELECT id FROM repos WHERE owner_id = ? AND name = ?');
        $stmt->execute([Auth::$user_id, $name]);
        if ($stmt->fetch()) {
            Response::error(409, 'Repository already exists');
        }

        DB::get()->prepare('INSERT INTO repos (owner_id, name) VALUES (?, ?)')
                 ->execute([Auth::$user_id, $name]);

        $dir = self::storagePath($name) . '/objects';
        if (!is_dir($dir) && !mkdir($dir, 0755, true)) {
            Response::error(500, 'Cannot create repository storage');
        }
    }

    /* 404s on "doesn't exist" AND "exists but belongs to someone else"
     * alike -- an authenticated user has no way to tell those apart. */
    public static function resolve(string $name): int
    {
        $name = self::sanitizeName($name);
        $stmt = DB::get()->prepare('SELECT id FROM repos WHERE owner_id = ? AND name = ?');
        $stmt->execute([Auth::$user_id, $name]);
        $row = $stmt->fetch();
        if (!$row) {
            Response::error(404, 'No such repository');
        }
        return (int)$row['id'];
    }
}
