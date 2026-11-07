<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Dumb, content-addressed object storage (docs/adr/09_ADR_VCS_Design.md:
 * the server "besitzt keinerlei Merge-Logik" -- more generally, it has no
 * VCS logic at all, just object/ref storage). Deliberately decoupled
 * from the client's own on-disk envelope (libcloudvcs/objstore.c's
 * [version][type][varint len][payload] loose-object format, or its pack
 * format): the wire body here is always the raw object payload, with
 * type carried as a separate query param / DB column and size as the
 * upload's Content-Length. The server never needs to parse the client's
 * local storage format, and the client never needs to know the server
 * stores things flat with no fan-out packing (v1: nothing here ever gets
 * "gc'd" the way a client repo's loose objects do -- see plan section 9,
 * a real filesystem's directory-entry cost doesn't justify packing on
 * this side, unlike FFS on the Amiga). */
class ObjectStore
{
    private static function objectPath(string $repoName, string $oid): string
    {
        return Repos::storagePath($repoName) . '/objects/' . substr($oid, 0, 2) . '/' . substr($oid, 2);
    }

    /* action=has: which of a list of oids the server already has, so a
     * pushing client only uploads what's actually missing (the
     * "Objekt-Negotiation" plan section 10 calls for). Pure DB lookup --
     * every stored object always has a row here, so no filesystem access
     * is needed just to answer this. */
    public static function has(string $repoName, array $oids): void
    {
        $repoId = Repos::resolve($repoName);
        if ($oids === []) {
            Response::ok(['have' => []]);
        }
        $ph = implode(',', array_fill(0, count($oids), '?'));
        $stmt = DB::get()->prepare(
            "SELECT oid FROM objects WHERE repo_id = ? AND oid IN ($ph)"
        );
        $stmt->execute(array_merge([$repoId], $oids));
        Response::ok(['have' => array_column($stmt->fetchAll(), 'oid')]);
    }

    public static function putObject(string $repoName, string $oid, int $type, string $data): void
    {
        if (!preg_match('/^[0-9a-f]{32}$/', $oid)) {
            Response::error(400, 'Malformed oid');
        }
        if (!in_array($type, [0, 1, 2], true)) {
            Response::error(400, 'Invalid type (expected 0=blob, 1=tree, 2=commit)');
        }
        if (strlen($data) > AMIVCS_MAX_UPLOAD) {
            Response::error(413, 'Object exceeds maximum allowed size');
        }
        if (md5($data) !== $oid) {
            Response::error(422, "oid mismatch: expected $oid");
        }

        $repoId = Repos::resolve($repoName);

        $stmt = DB::get()->prepare('SELECT id FROM objects WHERE repo_id = ? AND oid = ?');
        $stmt->execute([$repoId, $oid]);
        if ($stmt->fetch()) {
            Response::ok(['oid' => $oid]); /* already have it, content-addressed dedup */
        }

        $path = self::objectPath($repoName, $oid);
        $dir  = dirname($path);
        if (!is_dir($dir) && !@mkdir($dir, 0755, true) && !is_dir($dir)) {
            Response::error(500, 'Cannot create object directory');
        }
        if (file_put_contents($path, $data, LOCK_EX) === false) {
            Response::error(500, 'Write failed');
        }

        /* IGNORE: zwei gleichzeitige Uploads desselben Objekts (gleicher
         * Inhalt, gleiche Datei) sind beide erfolgreich, statt dass der
         * zweite am Duplicate Key mit 500 scheitert. */
        DB::get()->prepare(
            'INSERT IGNORE INTO objects (repo_id, oid, type, size_bytes) VALUES (?, ?, ?, ?)'
        )->execute([$repoId, $oid, $type, strlen($data)]);

        Response::ok(['oid' => $oid]);
    }

    public static function getObject(string $repoName, string $oid): void
    {
        if (!preg_match('/^[0-9a-f]{32}$/', $oid)) {
            Response::error(400, 'Malformed oid');
        }
        $repoId = Repos::resolve($repoName);

        $stmt = DB::get()->prepare('SELECT type FROM objects WHERE repo_id = ? AND oid = ?');
        $stmt->execute([$repoId, $oid]);
        $row = $stmt->fetch();
        if (!$row) {
            Response::error(404, 'No such object');
        }

        $data = @file_get_contents(self::objectPath($repoName, $oid));
        if ($data === false) {
            Response::error(500, 'Object row exists but its bytes are missing');
        }

        Response::rawObject($data, (int)$row['type']);
    }
}
