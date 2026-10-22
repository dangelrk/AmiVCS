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

        /* Echtes Compare-and-Swap in einer Anweisung: zwei gleichzeitige
         * Pushes mit demselben "old" koennen nicht beide gewinnen. */
        if ($old !== '') {
            $stmt = $db->prepare('UPDATE refs SET oid = ?, updated_at = NOW() WHERE repo_id = ? AND name = ? AND oid = ?');
            $stmt->execute([$new, $repoId, $name, $old]);
            $ok = $stmt->rowCount() === 1;
        } else {
            try {
                $db->prepare('INSERT INTO refs (repo_id, name, oid) VALUES (?, ?, ?)')
                   ->execute([$repoId, $name, $new]);
                $ok = true;
            } catch (PDOException $e) {
                if ($e->getCode() !== '23000') {
                    throw $e;
                }
                $ok = false;   /* gibt es schon (Duplicate Key) */
            }
        }
        if (!$ok) {
            $stmt = $db->prepare('SELECT oid FROM refs WHERE repo_id = ? AND name = ?');
            $stmt->execute([$repoId, $name]);
            $current = (string)($stmt->fetchColumn() ?: '');
            Response::error(409, "ref '$name' is at '$current', not '$old' -- pull and merge first");
        }
        $db->prepare('UPDATE repos SET pushed_at = NOW() WHERE id = ?')->execute([$repoId]);

        Response::ok(['name' => $name, 'oid' => $new]);
    }
}
