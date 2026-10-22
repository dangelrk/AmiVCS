<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

function visibilityBadge(array $repo): string
{
    return (int)$repo['is_public']
        ? '<span class="pill pill-public">' . icon('globe') . 'Öffentlich</span>'
        : '<span class="pill pill-private">' . icon('lock') . 'Privat</span>';
}

/* Eine Zeile der Repo-Listen; $showOwner bei "Entdecken" */
function repoListItem(array $r, bool $showOwner): string
{
    $when = $r['pushed_at'] ? 'Letzter Push ' . relTime(strtotime($r['pushed_at']))
                            : 'Angelegt ' . relTime(strtotime($r['created_at']));
    ob_start(); ?>
    <li class="repo-item">
      <a class="repo-name" href="<?= h(repoUrl($r)) ?>">
        <?= icon('repo') ?>
        <?php if ($showOwner): ?><span class="repo-owner"><?= h($r['owner']) ?> /</span><?php endif; ?>
        <strong><?= h($r['name']) ?></strong>
      </a>
      <?= visibilityBadge($r) ?>
      <?php if ($r['description'] !== ''): ?><p class="repo-desc"><?= h($r['description']) ?></p><?php endif; ?>
      <p class="repo-meta"><?= icon('branch') ?><?= (int)$r['branch_count'] ?> <?= (int)$r['branch_count'] === 1 ? 'Branch' : 'Branches' ?> · <?= h($when) ?></p>
    </li>
    <?php
    return (string)ob_get_clean();
}

function renderMyRepos(): string
{
    $repos = Repos::listOwn(Auth::$user_id);
    ob_start(); ?>
<div class="view-head">
  <h1 class="view-title"><?= icon('repo') ?>Meine Repositories</h1>
  <button type="button" class="btn btn-primary" data-open-dialog="dlgRepoNew"><?= icon('plus') ?>Neues Repository</button>
</div>
<?php if ($repos === []): ?>
  <div class="empty card"><?= icon('repo', 'ic empty-ic') ?><strong>Noch keine Repositories</strong>
    <span>Über „Neues Repository“ anlegen oder im Arbeitsverzeichnis <code>amivcs push name</code> ausführen.</span></div>
<?php else: ?>
  <ul class="repo-list"><?php foreach ($repos as $r) echo repoListItem($r, false); ?></ul>
<?php endif; ?>

<dialog id="dlgRepoNew" class="dlg dlg-wide">
  <form method="post" action="/web.php" class="form-grid">
    <?= csrfField() ?><input type="hidden" name="do" value="repo_create">
    <h2>Neues Repository</h2>
    <label for="nrName">Name</label>
    <input id="nrName" name="name" required maxlength="128" pattern="[A-Za-z0-9_\-][A-Za-z0-9_.\-]{0,127}" autocapitalize="none" spellcheck="false"
           title="Buchstaben, Ziffern, Punkt, Bindestrich, Unterstrich; nicht mit Punkt beginnen">
    <label for="nrDesc">Beschreibung</label>
    <input id="nrDesc" name="description" maxlength="255">
    <span></span><label class="check"><input type="checkbox" name="is_public" value="1"> Öffentlich – jeder kann es im Browser ansehen</label>
    <p class="form-hint">Danach im Arbeitsverzeichnis auf dem Amiga oder PC: <code>amivcs push name</code>. Ohne Anlegen hier geht das auch direkt – <code>push</code> legt fehlende Repositories selbst an (dann privat).</p>
    <div class="dlg-actions">
      <button type="button" class="btn" data-close-dialog>Abbrechen</button>
      <button type="submit" class="btn btn-primary">Anlegen</button>
    </div>
  </form>
</dialog>
<?php
    return (string)ob_get_clean();
}

function renderExplore(string $q): string
{
    $repos = Repos::listPublic($q);
    ob_start(); ?>
<h1 class="view-title"><?= icon('globe') ?>Öffentliche Repositories</h1>
<p class="view-sub"><?= $q !== '' ? 'Treffer für „' . h($q) . '“' : 'Alle öffentlich freigegebenen Projekte auf diesem Server.' ?></p>
<?php if ($repos === []): ?>
  <div class="empty card"><?= icon('globe', 'ic empty-ic') ?><strong><?= $q !== '' ? 'Keine Treffer' : 'Noch keine öffentlichen Repositories' ?></strong></div>
<?php else: ?>
  <ul class="repo-list"><?php foreach ($repos as $r) echo repoListItem($r, true); ?></ul>
<?php endif;
    return (string)ob_get_clean();
}
