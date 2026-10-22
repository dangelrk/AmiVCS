<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Seitengeruest wie bei AmiCloud: Kopfleiste (Logo, Repo-Suche,
 * Benutzermenue bzw. "Anmelden"), Navigation links, Inhalt rechts.
 * Anonyme Besucher sehen nur "Entdecken" (oeffentliche Repos). */
function renderLayout(string $title, string $active, string $body, string $query = ''): void
{
    $u       = Auth::$user;
    $name    = $u ? Users::displayName($u) : '';
    $flashes = takeFlashes();
    $nav = [];
    if ($u) {
        $nav['repos'] = ['repo', 'Meine Repositories', webUrl()];
    }
    $nav['explore'] = ['globe', 'Entdecken', webUrl(['view' => 'explore'])];
?><!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title><?= h($title) ?> – AmiVCS</title>
  <link rel="icon" href="/logo.svg" type="image/svg+xml">
  <link rel="stylesheet" href="<?= h(cssUrl()) ?>">
</head>
<body class="app">
<?= iconSprite() ?>
<header class="appbar">
  <button type="button" class="icon-btn nav-toggle" aria-label="Navigation" data-toggle-nav><?= icon('menu') ?></button>
  <a href="<?= h($u ? webUrl() : webUrl(['view' => 'explore'])) ?>" class="appbar-brand">
    <img src="/logo.svg" alt="" width="30" height="28">
    <span><span class="c-ami">Ami</span><span class="c-cloud">VCS</span></span>
  </a>
  <form class="appbar-search" method="get" action="/web.php" role="search">
    <input type="hidden" name="view" value="explore">
    <?= icon('search') ?>
    <input type="search" name="q" value="<?= h($query) ?>" placeholder="Öffentliche Repositories suchen …" aria-label="Repositories suchen">
  </form>
  <?php if ($u): ?>
  <div class="usermenu">
    <button type="button" class="avatar-btn" aria-haspopup="menu" aria-expanded="false" data-menu="usermenu">
      <span class="avatar"><?= h(initials($name)) ?></span>
      <span class="avatar-name"><?= h($name) ?></span>
    </button>
    <div class="menu" id="usermenu" role="menu" hidden>
      <div class="menu-head"><?= h($name) ?><small><?= h(Auth::$username) ?></small></div>
      <a role="menuitem" href="<?= h(webUrl(['view' => 'settings'])) ?>"><?= icon('gear') ?>Persönliche Einstellungen</a>
      <?php if (Auth::$is_admin): ?>
      <a role="menuitem" href="<?= h(webUrl(['view' => 'users'])) ?>"><?= icon('users') ?>Benutzer</a>
      <?php endif; ?>
      <form method="post" action="/web.php">
        <?= csrfField() ?><input type="hidden" name="do" value="logout">
        <button type="submit" role="menuitem"><?= icon('logout') ?>Abmelden</button>
      </form>
    </div>
  </div>
  <?php else: ?>
  <a class="btn btn-primary appbar-login" href="<?= h(webUrl(['view' => 'login'])) ?>"><?= icon('login') ?>Anmelden</a>
  <?php endif; ?>
</header>

<div class="shell">
  <nav class="sidebar" id="sidebar">
    <ul class="nav">
      <?php foreach ($nav as $key => [$ic, $label, $url]): ?>
      <li><a href="<?= h($url) ?>"<?= $active === $key ? ' class="active" aria-current="page"' : '' ?>><?= icon($ic) ?><?= h($label) ?></a></li>
      <?php endforeach; ?>
    </ul>
    <?php if ($u): ?>
    <div class="nav-bottom">
      <ul class="nav">
        <li><a href="<?= h(webUrl(['view' => 'settings'])) ?>"<?= $active === 'settings' ? ' class="active"' : '' ?>><?= icon('gear') ?>Einstellungen</a></li>
        <?php if (Auth::$is_admin): ?>
        <li><a href="<?= h(webUrl(['view' => 'users'])) ?>"<?= $active === 'users' ? ' class="active"' : '' ?>><?= icon('users') ?>Benutzer</a></li>
        <?php endif; ?>
      </ul>
    </div>
    <?php endif; ?>
  </nav>

  <main class="content">
    <?php foreach ($flashes as $f): ?>
    <div class="flash flash-<?= h($f['type']) ?>" role="status"><?= h($f['msg']) ?><button type="button" class="icon-btn" data-dismiss aria-label="Schließen"><?= icon('close') ?></button></div>
    <?php endforeach; ?>
    <?= $body ?>
  </main>
</div>

<footer class="appfooter">AmiVCS v<?= h(AMIVCS_VERSION) ?></footer>
<script src="<?= h(jsUrl()) ?>"></script>
</body>
</html>
<?php
}

function renderLogin(string $error): void
{
?><!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Anmelden – AmiVCS</title>
  <link rel="icon" href="/logo.svg" type="image/svg+xml">
  <link rel="stylesheet" href="<?= h(cssUrl()) ?>">
</head>
<body class="login-page">
  <div class="login-box">
    <a href="/" class="login-logo"><img src="/logo.svg" alt="" width="96" height="89"></a>
    <h1><span class="c-ami">Ami</span><span class="c-cloud">VCS</span></h1>
    <?php if ($error !== ''): ?>
      <div class="msg-error"><?= h($error) ?></div>
    <?php endif; ?>
    <form method="post" action="/web.php">
      <?= csrfField() ?><input type="hidden" name="do" value="login">
      <label>Benutzer
        <input type="text" name="username" autofocus autocomplete="username" autocapitalize="none" spellcheck="false">
      </label>
      <label>Passwort
        <input type="password" name="password" autocomplete="current-password">
      </label>
      <button type="submit">Anmelden</button>
    </form>
    <p class="login-alt"><a href="<?= h(webUrl(['view' => 'explore'])) ?>">Öffentliche Repositories ansehen</a></p>
  </div>
</body>
</html>
<?php
}
