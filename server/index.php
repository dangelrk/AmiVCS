<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
define('AMIVCS_SERVER', 1);

/* Startseite: Logo, Kurzbeschreibung, die neuesten oeffentlichen
 * Repositories. Die Clients sprechen nur api.php an, die Weboberflaeche
 * liegt unter web.php. Faellt die Datenbank aus, erscheint die Seite
 * trotzdem (nur ohne Repo-Liste). */
require __DIR__ . '/config.php';
require __DIR__ . '/includes/Version.php';
require __DIR__ . '/includes/DB.php';
require __DIR__ . '/includes/Auth.php';
require __DIR__ . '/includes/Repos.php';
require __DIR__ . '/includes/web/helpers.php';
date_default_timezone_set(defined('AMIVCS_TIMEZONE') ? AMIVCS_TIMEZONE : 'Europe/Berlin');

/* Angemeldet? Nur nachsehen, wenn schon ein Session-Cookie existiert */
$loggedIn = false;
if (isset($_COOKIE[session_name()])) {
    session_start(['read_and_close' => true]);
    $loggedIn = isset($_SESSION['user_id']);
}

$repos = [];
try {
    $repos = Repos::listPublic('', 6);
} catch (Throwable $e) {
    $repos = [];
}
?><!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>AmiVCS v<?= AMIVCS_VERSION ?></title>
  <meta name="description" content="AmiVCS – kleine Versionsverwaltung für den Amiga und den PC">
  <link rel="icon" href="/logo.svg" type="image/svg+xml">
  <link rel="stylesheet" href="<?= h(cssUrl()) ?>">
</head>
<body class="landing">
  <?= iconSprite() ?>
  <div class="copper" aria-hidden="true"></div>
  <div class="landing-bg" aria-hidden="true"><?php readfile(__DIR__ . '/logo.svg'); ?></div>

  <header class="hero">
    <h1 class="hero-title"><span class="c-ami">Ami</span><span class="c-cloud">VCS</span></h1>
    <p class="hero-tagline">Versionsverwaltung für den Amiga – und den PC.</p>
    <div class="hero-actions">
      <a href="/web.php" class="btn-hero"><?= $loggedIn ? 'Zu meinen Repositories' : 'Anmelden' ?></a>
      <a href="/web.php?view=explore" class="btn-hero-alt">Öffentliche Repositories</a>
    </div>
  </header>

  <main class="features">
    <section class="feature">
      <h2>amivcs</h2>
      <p>Kommandozeile mit <code>init</code>, <code>add</code>, <code>commit</code>,
         <code>log</code>, <code>branch</code>, <code>merge</code> – läuft schon auf dem 68000.</p>
    </section>
    <section class="feature">
      <h2>amivcs-gui</h2>
      <p>Grafische Oberfläche im Stil von git-gui: Vormerken, Commit, Branches und
         Historie mit Graph – MUI auf dem Amiga, Qt auf dem PC.</p>
    </section>
    <section class="feature">
      <h2>Lokal zuerst</h2>
      <p>Fast alles passiert im Arbeitsverzeichnis. Der Server wird nur für
         <code>clone</code>, <code>push</code> und <code>pull</code> gebraucht.</p>
    </section>
    <section class="feature">
      <h2>Im Browser</h2>
      <p>Dateien, Historie und Änderungen jedes Commits ansehen; öffentliche
         Projekte auch ohne Konto.</p>
    </section>

    <section class="setup">
      <h2>Einrichten</h2>
      <p>In AMIVCSPrefs bzw. <code>amivcs.cfg</code> diesen Server eintragen:</p>
      <pre>server=<?= h(serverUrl()) . "\n" ?>
user=…
pass=…</pre>
    </section>
  </main>

  <?php if ($repos !== []): ?>
  <section class="landing-repos">
    <h2>Neueste öffentliche Repositories</h2>
    <ul class="repo-list">
      <?php foreach ($repos as $r): ?>
      <li class="repo-item">
        <a class="repo-name" href="<?= h(repoUrl($r)) ?>"><?= icon('repo') ?><span class="repo-owner"><?= h($r['owner']) ?> /</span><strong><?= h($r['name']) ?></strong></a>
        <?php if ($r['description'] !== ''): ?><p class="repo-desc"><?= h($r['description']) ?></p><?php endif; ?>
      </li>
      <?php endforeach; ?>
    </ul>
  </section>
  <?php endif; ?>

  <footer class="landing-footer">
    AmiVCS v<?= h(AMIVCS_VERSION) ?> · GPL-3.0 · AmigaOS 3.x, bsdsocket + AmiSSL
  </footer>
</body>
</html>
