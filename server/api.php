<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
define('AMIVCS_SERVER', 1);

require __DIR__ . '/config.php';

/* Enforce HTTPS -- mirrors AmiCloud's api.php. AMIVCS_ALLOW_HTTP is for
 * local development only (php -S can't do TLS) -- never set in production. */
$https = $_SERVER['HTTPS'] ?? '';
if ((empty($https) || $https === 'off')
    && !(defined('AMIVCS_ALLOW_HTTP') && AMIVCS_ALLOW_HTTP)) {
    http_response_code(403);
    header('Content-Type: application/json');
    echo '{"status":"error","code":403,"message":"HTTPS required"}';
    exit;
}

require __DIR__ . '/includes/Version.php';
require __DIR__ . '/includes/Response.php';
require __DIR__ . '/includes/DB.php';
require __DIR__ . '/includes/Auth.php';
require __DIR__ . '/includes/Repos.php';
require __DIR__ . '/includes/ObjectStore.php';
require __DIR__ . '/includes/RefStore.php';

Auth::requireBasicAuth();

$action = trim($_GET['action'] ?? '');
$repo   = trim($_GET['repo'] ?? '');

switch ($action) {
    case 'status':
        Response::ok(['version' => AMIVCS_VERSION]);
        break;

    case 'create-repo':
        Repos::create($repo);
        Response::ok(['repo' => $repo]);
        break;

    case 'refs':
        RefStore::list($repo);
        break;

    case 'update-ref':
        RefStore::updateRef(
            $repo,
            (string)($_GET['name'] ?? ''),
            (string)($_GET['old'] ?? ''),
            (string)($_GET['new'] ?? '')
        );
        break;

    case 'has':
        $oids = array_values(array_filter(explode(',', (string)($_GET['oids'] ?? ''))));
        ObjectStore::has($repo, $oids);
        break;

    case 'put-object':
        ObjectStore::putObject(
            $repo,
            (string)($_GET['oid'] ?? ''),
            (int)($_GET['type'] ?? -1),
            (string)file_get_contents('php://input')
        );
        break;

    case 'get-object':
        ObjectStore::getObject($repo, (string)($_GET['oid'] ?? ''));
        break;

    default:
        Response::error(400, 'Unknown action: ' . htmlspecialchars($action));
}
