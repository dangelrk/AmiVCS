<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

class RefStore
{
    public static function list(string $repoName): void
    {
        $repoId = Repos::resolve($repoName);
        $stmt = DB::get()->prepare('SELECT name, oid FROM refs WHERE repo_id = ?');
        $stmt->execute([$repoId]);
        Response::ok(['refs' => $stmt->fetchAll()]);
    }

    /* Compare-and-swap ref update (plan section 7): old must match the
     * ref's CURRENT oid -- empty string means "must not exist yet" -- or
     * this refuses with 409. The pushing client is expected to pull,
     * merge locally and retry; the server never merges (ADR 09). */
    public static function updateRef(string $repoName, string $name, string $old, string $new): void
    {
        if (!preg_match('#^refs/heads/[A-Za-z0-9_.\-/]{1,100}$#', $name)) {
            Response::error(400, 'Invalid ref name');
        }
        if ($new !== '' && !preg_match('/^[0-9a-f]{32}$/', $new)) {
            Response::error(400, 'Malformed new oid');
        }
        if ($old !== '' && !preg_match('/^[0-9a-f]{32}$/', $old)) {
            Response::error(400, 'Malformed old oid');
        }

        $repoId = Repos::resolve($repoName);
        $db = DB::get();

        $stmt = $db->prepare('SELECT oid FROM refs WHERE repo_id = ? AND name = ?');
        $stmt->execute([$repoId, $name]);
        $row = $stmt->fetch();
        $current = $row ? $row['oid'] : '';

        if ($current !== $old) {
            Response::error(409, "ref '$name' is at '$current', not '$old' -- pull and merge first");
        }

        if ($row) {
            $db->prepare('UPDATE refs SET oid = ?, updated_at = NOW() WHERE repo_id = ? AND name = ?')
               ->execute([$new, $repoId, $name]);
        } else {
            $db->prepare('INSERT INTO refs (repo_id, name, oid) VALUES (?, ?, ?)')
               ->execute([$repoId, $name, $new]);
        }

        Response::ok(['name' => $name, 'oid' => $new]);
    }
}
