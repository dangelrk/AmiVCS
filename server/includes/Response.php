<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

class Response
{
    public static function json(array $data, int $code = 200): void
    {
        http_response_code($code);
        header('Content-Type: application/json; charset=utf-8');
        echo json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        exit;
    }

    public static function ok(array $extra = []): void
    {
        self::json(array_merge(['status' => 'ok'], $extra));
    }

    public static function error(int $code, string $message): void
    {
        self::json(['status' => 'error', 'code' => $code, 'message' => $message], $code);
    }

    /* Raw object bytes -- used by ObjectStore::getObject(). AMIVCS_TYPE
     * header carries the object type (0=blob/1=tree/2=commit) since the
     * body itself is just the payload, with none of the client's own
     * on-disk envelope (plan: the wire format and the local .amivcs
     * storage format are deliberately decoupled). */
    public static function rawObject(string $data, int $type): void
    {
        header('X-Amivcs-Type: ' . $type);
        header('Content-Type: application/octet-stream');
        header('Content-Length: ' . strlen($data));
        echo $data;
        exit;
    }
}
