<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
session_set_cookie_params([
    'lifetime' => 0,
    'path'     => '/',
    'secure'   => true,
    'httponly' => true,
    'samesite' => 'Strict',
]);
session_start();
define('AMIVCS_SERVER', 1);

require __DIR__ . '/config.php';
require __DIR__ . '/includes/Version.php';
require __DIR__ . '/includes/DB.php';
require __DIR__ . '/includes/Auth.php';
require __DIR__ . '/includes/Repos.php';
require __DIR__ . '/includes/ObjectDecoder.php';
require __DIR__ . '/includes/GraphLayout.php';

/* Auth::requireBasicAuth()/Response::* are api.php's stateless, JSON-only
 * contract for the C clients -- this file is a session/cookie-based
 * human UI and never includes Response.php (every one of its methods
 * calls exit(), incompatible with rendering an HTML page) or calls
 * Auth::requireBasicAuth() (see the login section below instead). */

/* ── Helpers ─────────────────────────────────────────────────────────── */

function h(string $s): string
{
    return htmlspecialchars($s, ENT_QUOTES, 'UTF-8');
}

function formatDate(int $epochSeconds): string
{
    return gmdate('Y-m-d H:i:s', $epochSeconds) . ' UTC';
}

/* AMIVCS_STORAGE_PATH/<owner>/<repo>/objects/<2-hex>/<30-hex> -- same
 * fan-out formula as ObjectStore.php's private objectPath(), duplicated
 * here rather than exposed publicly there since that class's contract is
 * the stateless API (has/put-object/get-object), not general path
 * lookup for a different caller. */
function objectFilePath(string $repoName, string $oid): string
{
    return Repos::storagePath($repoName) . '/objects/' . substr($oid, 0, 2) . '/' . substr($oid, 2);
}

/* Reads and decodes a commit object straight off disk for the given
 * repo. Returns null (skip, don't fail the whole history render) if the
 * object is missing, unreadable, or not actually a commit -- a
 * defensively-coded display path, not a place to surface a hard error
 * over one bad object. */
function makeCommitReader(int $repoId, string $repoName): callable
{
    return function (string $oid) use ($repoId, $repoName): ?array {
        static $cache = [];
        if (array_key_exists($oid, $cache)) return $cache[$oid];

        $stmt = DB::get()->prepare('SELECT type FROM objects WHERE repo_id = ? AND oid = ?');
        $stmt->execute([$repoId, $oid]);
        $row = $stmt->fetch();
        if (!$row || (int)$row['type'] !== 2) { return $cache[$oid] = null; }

        $data = @file_get_contents(objectFilePath($repoName, $oid));
        if ($data === false) { return $cache[$oid] = null; }

        try {
            return $cache[$oid] = ObjectDecoder::decodeCommit($data);
        } catch (RuntimeException $e) {
            return $cache[$oid] = null;
        }
    };
}

/* ── HTTPS enforce (AMIVCS_ALLOW_HTTP: local development only) ───────── */
if ((empty($_SERVER['HTTPS']) || $_SERVER['HTTPS'] === 'off')
    && !(defined('AMIVCS_ALLOW_HTTP') && AMIVCS_ALLOW_HTTP)) {
    header('Location: https://' . $_SERVER['HTTP_HOST'] . $_SERVER['REQUEST_URI']);
    exit;
}

/* ── Logout ──────────────────────────────────────────────────────────── */
if (isset($_GET['logout'])) {
    session_destroy();
    header('Location: /web.php');
    exit;
}

/* ── Login ───────────────────────────────────────────────────────────── */
$loginError = '';

if (!isset($_SESSION['user_id'])) {
    if (empty($_SESSION['csrf'])) {
        $_SESSION['csrf'] = bin2hex(random_bytes(16));
    }

    if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['username'])) {
        if (!hash_equals($_SESSION['csrf'], $_POST['csrf'] ?? '')) {
            $loginError = 'Invalid request.';
        } else {
            $stmt = DB::get()->prepare(
                'SELECT id, pass_hash FROM users WHERE username = ? AND active = 1 LIMIT 1'
            );
            $stmt->execute([trim($_POST['username'])]);
            $row = $stmt->fetch();

            if ($row && password_verify($_POST['password'] ?? '', $row['pass_hash'])) {
                session_regenerate_id(true);
                $_SESSION['user_id']  = (int)$row['id'];
                $_SESSION['username'] = trim($_POST['username']);
                $_SESSION['csrf']     = bin2hex(random_bytes(16));
                header('Location: /web.php');
                exit;
            }
            $loginError = 'Invalid credentials.';
        }
    }

    renderLogin($loginError);
    exit;
}

/* ── Auth state ──────────────────────────────────────────────────────── */
Auth::$user_id  = $_SESSION['user_id'];
Auth::$username = $_SESSION['username'];

/* ── Repo detail (branches + full multi-branch history) ───────────────── */
$repoName = trim((string)($_GET['repo'] ?? ''));
if ($repoName !== '') {
    $stmt = DB::get()->prepare('SELECT id, name, created_at FROM repos WHERE owner_id = ? AND name = ?');
    $stmt->execute([Auth::$user_id, $repoName]);
    $repo = $stmt->fetch();
    if (!$repo) {
        header('Location: /web.php');
        exit;
    }
    $repoId = (int)$repo['id'];

    $stmt = DB::get()->prepare('SELECT name, oid FROM refs WHERE repo_id = ? ORDER BY name');
    $stmt->execute([$repoId]);
    $refs = $stmt->fetchAll();

    $graph = GraphLayout::build($refs, makeCommitReader($repoId, $repoName));

    renderRepoDetail($repo, $refs, $graph);
    exit;
}

/* ── Repo list ───────────────────────────────────────────────────────── */
$stmt = DB::get()->prepare(
    'SELECT r.id, r.name, r.created_at,
            (SELECT COUNT(*) FROM refs WHERE repo_id = r.id) AS branch_count
       FROM repos r
      WHERE r.owner_id = ?
      ORDER BY r.name'
);
$stmt->execute([Auth::$user_id]);
$repos = $stmt->fetchAll();

renderRepoList($repos);


/* ════════════════════════════════════════════════════════════════════════
   Render: Login
   ════════════════════════════════════════════════════════════════════════ */
function renderLogin(string $error): void
{
    $csrf = $_SESSION['csrf'];
?><!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AMIVCS v<?= AMIVCS_VERSION ?> — Login</title>
  <link rel="stylesheet" href="/web.css">
</head>
<body class="login-page">
  <div class="login-box">
    <h1><span class="c-ami">AMI</span><span class="c-vcs">VCS</span></h1>
    <?php if ($error !== ''): ?>
      <div class="msg-error"><?= h($error) ?></div>
    <?php endif; ?>
    <form method="post" action="/web.php">
      <input type="hidden" name="csrf" value="<?= h($csrf) ?>">
      <label>Username
        <input type="text" name="username" autofocus autocomplete="username">
      </label>
      <label>Password
        <input type="password" name="password" autocomplete="current-password">
      </label>
      <button type="submit">Sign in</button>
    </form>
  </div>
</body>
</html>
<?php
}


/* ════════════════════════════════════════════════════════════════════════
   Render: Repo list
   ════════════════════════════════════════════════════════════════════════ */
function renderRepoList(array $repos): void
{
?><!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AMIVCS v<?= AMIVCS_VERSION ?> — Repositories</title>
  <link rel="stylesheet" href="/web.css">
</head>
<body>
  <header class="topbar">
    <a href="/web.php" class="brand"><span class="c-ami">AMI</span><span class="c-vcs">VCS</span></a>
    <span class="userinfo">
      <?= h(Auth::$username) ?>
      <a href="/web.php?logout" class="btn-logout">Sign out</a>
    </span>
  </header>
  <main>
    <h2>Your repositories</h2>
    <table class="repotable">
      <thead>
        <tr><th>Name</th><th>Branches</th><th>Created</th></tr>
      </thead>
      <tbody>
        <?php foreach ($repos as $r): ?>
        <tr>
          <td><a href="/web.php?repo=<?= urlencode($r['name']) ?>"><?= h($r['name']) ?></a></td>
          <td class="col-meta"><?= (int)$r['branch_count'] ?></td>
          <td class="col-meta"><?= h(substr((string)$r['created_at'], 0, 16)) ?></td>
        </tr>
        <?php endforeach; ?>
        <?php if (empty($repos)): ?>
        <tr><td colspan="3" class="empty">No repositories yet -- create one via the amivcs CLI/GUI (`amivcs push`).</td></tr>
        <?php endif; ?>
      </tbody>
    </table>
  </main>
  <footer class="pagefooter">AMIVCS v<?= AMIVCS_VERSION ?></footer>
</body>
</html>
<?php
}


/* ════════════════════════════════════════════════════════════════════════
   Render: Repo detail (branches + full multi-branch history)
   ════════════════════════════════════════════════════════════════════════ */
function renderRepoDetail(array $repo, array $refs, array $graph): void
{
    $maxLanes = max(1, $graph['max_lanes']);
?><!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AMIVCS v<?= AMIVCS_VERSION ?> — <?= h($repo['name']) ?></title>
  <link rel="stylesheet" href="/web.css">
</head>
<body>
  <header class="topbar">
    <a href="/web.php" class="brand"><span class="c-ami">AMI</span><span class="c-vcs">VCS</span></a>
    <span class="userinfo">
      <?= h(Auth::$username) ?>
      <a href="/web.php?logout" class="btn-logout">Sign out</a>
    </span>
  </header>
  <main>
    <nav class="breadcrumb"><a href="/web.php">Repositories</a> <span class="sep">›</span> <?= h($repo['name']) ?></nav>

    <h2><?= h($repo['name']) ?></h2>

    <h3>Branches</h3>
    <ul class="branchlist">
      <?php foreach ($refs as $r): ?>
      <li>
        <span class="branch-name"><?= h(preg_replace('#^refs/heads/#', '', $r['name'])) ?></span>
        <span class="branch-oid"><?= h(substr($r['oid'], 0, 8)) ?></span>
      </li>
      <?php endforeach; ?>
      <?php if (empty($refs)): ?>
      <li class="empty">No branches yet</li>
      <?php endif; ?>
    </ul>

    <h3>History</h3>
    <table class="historytable">
      <thead>
        <tr><th class="col-graph">Graph</th><th>OID</th><th>Author</th><th>Date</th><th>Message</th></tr>
      </thead>
      <tbody>
        <?php foreach ($graph['rows'] as $row): ?>
        <tr>
          <td class="col-graph">
            <svg width="<?= $maxLanes * 16 ?>" height="20" class="graph-svg">
              <?php foreach ($row['active_lanes'] as $lane): ?>
                <?php if ($lane !== $row['lane']): ?>
                <line x1="<?= $lane * 16 + 8 ?>" y1="0" x2="<?= $lane * 16 + 8 ?>" y2="20" class="graph-line" />
                <?php endif; ?>
              <?php endforeach; ?>
              <?php foreach ($row['parent_lanes'] as $plane): ?>
                <line x1="<?= $row['lane'] * 16 + 8 ?>" y1="10" x2="<?= $plane * 16 + 8 ?>" y2="20" class="graph-line" />
              <?php endforeach; ?>
              <circle cx="<?= $row['lane'] * 16 + 8 ?>" cy="10" r="4" class="graph-dot" />
            </svg>
          </td>
          <td class="col-meta"><?= h(substr($row['oid'], 0, 8)) ?></td>
          <td class="col-meta"><?= h($row['author']) ?></td>
          <td class="col-meta"><?= h(formatDate($row['mtime'])) ?></td>
          <td>
            <?php if ($row['decoration'] !== ''): ?><span class="deco">[<?= h($row['decoration']) ?>]</span> <?php endif; ?>
            <?= h($row['message']) ?>
          </td>
        </tr>
        <?php endforeach; ?>
        <?php if (empty($graph['rows'])): ?>
        <tr><td colspan="5" class="empty">No commits yet</td></tr>
        <?php endif; ?>
      </tbody>
    </table>
  </main>
  <footer class="pagefooter">AMIVCS v<?= AMIVCS_VERSION ?></footer>
</body>
</html>
<?php
}
