<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Repo name -> storage path / DB row resolution. Every repo has exactly
 * one owner (docs/adr/09_ADR_VCS_Design.md), a repo name is only unique
 * per owner. The API (api.php) always works on the authenticated user's
 * own repos; the WebUI addresses repos as owner/name and may also show
 * other users' public ones (forView(), docs/adr/10_ADR_WebUI.md). */
class Repos
{
    const NAME_RE = '/^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,127}$/';

    /* Leading dot rejected: "." and ".." would otherwise resolve to the
     * owner's storage root or a sibling user's directory. */
    public static function validName(string $name): bool
    {
        return (bool)preg_match(self::NAME_RE, $name);
    }

    private static function sanitizeName(string $name): string
    {
        if (!self::validName($name)) {
            Response::error(400, 'Invalid repo name');
        }
        return $name;
    }

    public static function storageDir(string $owner, string $name): string
    {
        return rtrim(AMIVCS_STORAGE_PATH, '/') . '/' . $owner . '/' . $name;
    }

    public static function storagePath(string $name): string
    {
        return self::storageDir(Auth::$username, self::sanitizeName($name));
    }

    /* Shared by the API's create-repo and the WebUI. null = ok, else a
     * message; $code receives the matching HTTP status for the API. */
    public static function createFor(int $uid, string $owner, string $name, string $desc,
                                     bool $public, int &$code = 0): ?string
    {
        if (!self::validName($name)) {
            $code = 400;
            return 'Ungültiger Name: Buchstaben, Ziffern, ".", "-" und "_", nicht mit "." beginnend.';
        }
        if (mb_strlen($desc, 'UTF-8') > 255 || preg_match('/[\x00-\x1f\x7f]/', $desc)) {
            $code = 400;
            return 'Die Beschreibung darf höchstens 255 Zeichen lang sein.';
        }
        $stmt = DB::get()->prepare('SELECT id FROM repos WHERE owner_id = ? AND name = ?');
        $stmt->execute([$uid, $name]);
        if ($stmt->fetch()) {
            $code = 409;
            return "Das Repository $name gibt es schon.";
        }
        $dir = self::storageDir($owner, $name) . '/objects';
        if (!is_dir($dir) && !@mkdir($dir, 0755, true)) {
            $code = 500;
            return 'Speicherordner konnte nicht angelegt werden.';
        }
        DB::get()->prepare('INSERT INTO repos (owner_id, name, description, is_public) VALUES (?, ?, ?, ?)')
                 ->execute([$uid, $name, $desc, $public ? 1 : 0]);
        return null;
    }

    public static function create(string $name): void
    {
        $code = 0;
        $err  = self::createFor(Auth::$user_id, Auth::$username, $name, '', false, $code);
        if ($err !== null) {
            Response::error($code, $code === 409 ? 'Repository already exists'
                                 : ($code === 400 ? 'Invalid repo name' : 'Cannot create repository storage'));
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

    /* ── WebUI ─────────────────────────────────────────────────────────── */

    const VIEW_COLS = 'r.id, r.owner_id, r.name, r.description, r.is_public, r.default_branch,
                       r.created_at, r.pushed_at, u.username AS owner, u.display_name AS owner_display,
                       (SELECT COUNT(*) FROM refs f WHERE f.repo_id = r.id) AS branch_count';

    /* The single access check for every repo page: the repo is returned
     * only if it is public or belongs to the signed-in user (Auth::$user_id,
     * 0 = anonymous). Private and nonexistent look the same (null -> 404),
     * and an inactive owner hides even public repos. */
    public static function forView(string $owner, string $name): ?array
    {
        if (!self::validName($name)) {
            return null;
        }
        $stmt = DB::get()->prepare(
            'SELECT ' . self::VIEW_COLS . ' FROM repos r JOIN users u ON u.id = r.owner_id
              WHERE u.username = ? AND r.name = ? AND u.active = 1
                AND (r.is_public = 1 OR r.owner_id = ?)'
        );
        $stmt->execute([substr($owner, 0, 64), $name, Auth::$user_id]);
        $row = $stmt->fetch();
        if (!$row) {
            return null;
        }
        $row['is_owner'] = Auth::$user_id !== 0 && (int)$row['owner_id'] === Auth::$user_id;
        return $row;
    }

    public static function listOwn(int $uid): array
    {
        $stmt = DB::get()->prepare(
            'SELECT ' . self::VIEW_COLS . ' FROM repos r JOIN users u ON u.id = r.owner_id
              WHERE r.owner_id = ? ORDER BY COALESCE(r.pushed_at, r.created_at) DESC, r.name'
        );
        $stmt->execute([$uid]);
        return $stmt->fetchAll();
    }

    public static function listPublic(string $q = '', int $limit = 200): array
    {
        $sql  = 'SELECT ' . self::VIEW_COLS . ' FROM repos r JOIN users u ON u.id = r.owner_id
                  WHERE r.is_public = 1 AND u.active = 1';
        $args = [];
        if ($q !== '') {
            $like  = '%' . str_replace(['\\', '%', '_'], ['\\\\', '\\%', '\\_'], $q) . '%';
            $sql  .= ' AND (r.name LIKE ? OR r.description LIKE ? OR u.username LIKE ?)';
            $args  = [$like, $like, $like];
        }
        $sql .= ' ORDER BY COALESCE(r.pushed_at, r.created_at) DESC LIMIT ' . max(1, min(500, $limit));
        $stmt = DB::get()->prepare($sql);
        $stmt->execute($args);
        return $stmt->fetchAll();
    }

    public static function updateSettings(int $repoId, string $desc, bool $public, string $defaultBranch): ?string
    {
        if (mb_strlen($desc, 'UTF-8') > 255 || preg_match('/[\x00-\x1f\x7f]/', $desc)) {
            return 'Die Beschreibung darf höchstens 255 Zeichen lang sein.';
        }
        if (!preg_match('#^refs/heads/[A-Za-z0-9_.\-/]{1,100}$#', $defaultBranch)) {
            return 'Ungültiger Standard-Branch.';
        }
        DB::get()->prepare('UPDATE repos SET description = ?, is_public = ?, default_branch = ? WHERE id = ?')
                 ->execute([$desc, $public ? 1 : 0, $defaultBranch, $repoId]);
        return null;
    }

    /* Removes the DB rows (objects/refs via CASCADE) and the storage
     * directory, after double-checking that it sits inside the owner's
     * directory under AMIVCS_STORAGE_PATH. */
    public static function delete(array $repo): ?string
    {
        $dir  = self::storageDir($repo['owner'], $repo['name']);
        $base = realpath(rtrim(AMIVCS_STORAGE_PATH, '/') . '/' . $repo['owner']);
        if (is_dir($dir)) {
            if ($base === false || !self::validName($repo['name']) || realpath(dirname($dir)) !== $base) {
                return 'Speicherordner liegt nicht am erwarteten Ort – Löschen abgebrochen.';
            }
            self::deleteTree($dir);
        }
        DB::get()->prepare('DELETE FROM repos WHERE id = ?')->execute([(int)$repo['id']]);
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
