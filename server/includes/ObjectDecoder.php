<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* ObjectDecoder.php -- read-only decode of AMIVCS's Tree/Commit binary
 * object payloads (libcloudvcs/src/object.c's exact on-disk encoding),
 * for the WebUI's history view only. A narrow, deliberate exception to
 * ADR 09 (the server owns no VCS *decision* logic -- merge/ancestry
 * computation stays 100% client-side): this only decodes already-stored
 * bytes for display, it never writes anything or makes any VCS choice.
 * Mirrors object.c's varint/tree/commit format field-for-field -- keep
 * both in sync if that format ever changes.
 *
 * Payloads here are exactly what the client uploaded via put-object
 * (ObjectStore.php's putObject()): no envelope/header to strip, OIDs
 * inside the payload are 16 raw MD5 bytes (not the 32-hex-char string
 * used everywhere else in this codebase, e.g. the `objects.oid` column
 * or put-object's own query param) -- bin2hex() them before use. */
class ObjectDecoder
{
    private static function readVarint(string $data, int &$pos): int
    {
        $result = 0;
        $shift = 0;
        while (true) {
            if ($pos >= strlen($data)) {
                throw new RuntimeException('truncated varint');
            }
            $byte = ord($data[$pos]);
            $pos++;
            $result |= ($byte & 0x7f) << $shift;
            if (($byte & 0x80) === 0) break;
            $shift += 7;
        }
        return $result;
    }

    /* Decodes a VCS_OBJ_TREE payload into a list of
     * ['name'=>string,'is_dir'=>bool,'oid'=>32-hex-char string]. */
    public static function decodeTree(string $data): array
    {
        if (strlen($data) < 1 || ord($data[0]) !== 1) {
            throw new RuntimeException('unsupported tree format version');
        }
        $pos = 1;
        $count = self::readVarint($data, $pos);

        $entries = [];
        for ($i = 0; $i < $count; $i++) {
            if ($pos + 2 > strlen($data)) {
                throw new RuntimeException('truncated tree entry');
            }
            $isDir = ord($data[$pos]) !== 0;
            $pos++;
            $namelen = ord($data[$pos]);
            $pos++;
            if ($pos + $namelen + 16 > strlen($data)) {
                throw new RuntimeException('truncated tree entry');
            }
            $name = substr($data, $pos, $namelen);
            $pos += $namelen;
            $oidRaw = substr($data, $pos, 16);
            $pos += 16;

            $entries[] = ['name' => $name, 'is_dir' => $isDir, 'oid' => bin2hex($oidRaw)];
        }
        return $entries;
    }

    /* Decodes a VCS_OBJ_COMMIT payload into
     * ['tree'=>hex,'parents'=>[hex,...],'mtime'=>int,'author'=>string,'message'=>string]. */
    public static function decodeCommit(string $data): array
    {
        if (strlen($data) < 1 || ord($data[0]) !== 1) {
            throw new RuntimeException('unsupported commit format version');
        }
        $pos = 1;
        if ($pos + 16 > strlen($data)) {
            throw new RuntimeException('truncated commit');
        }
        $tree = bin2hex(substr($data, $pos, 16));
        $pos += 16;

        $nparents = ord($data[$pos]);
        $pos++;
        $parents = [];
        for ($i = 0; $i < $nparents; $i++) {
            if ($pos + 16 > strlen($data)) {
                throw new RuntimeException('truncated commit parent');
            }
            $parents[] = bin2hex(substr($data, $pos, 16));
            $pos += 16;
        }

        $mtime = self::readVarint($data, $pos);

        if ($pos >= strlen($data)) {
            throw new RuntimeException('truncated commit author length');
        }
        $authorlen = ord($data[$pos]);
        $pos++;
        if ($pos + $authorlen > strlen($data)) {
            throw new RuntimeException('truncated commit author');
        }
        $author = substr($data, $pos, $authorlen);
        $pos += $authorlen;

        $msglen = self::readVarint($data, $pos);
        if ($pos + $msglen > strlen($data)) {
            throw new RuntimeException('truncated commit message');
        }
        $message = substr($data, $pos, $msglen);

        return ['tree' => $tree, 'parents' => $parents, 'mtime' => $mtime, 'author' => $author, 'message' => $message];
    }
}
