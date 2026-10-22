<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Repository-Seiten (Code, Datei, Historie, Commit, Branches,
 * Einstellungen). Alle bekommen ein per Repos::forView() geprueftes Repo
 * und einen RepoReader; Pfade sind Tree-Pfade ohne fuehrenden "/". */

const DISPLAY_MAX_BYTES = 1048576;  // groessere Dateien nur zum Download
const DIFF_MAX_BYTES    = 524288;
const DIFF_MAX_LINES    = 5000;
const HISTORY_PAGE      = 50;
const TREE_MAX_ENTRIES  = 2000;     // Code-Ansicht zeigt hoechstens so viele Eintraege
const DIFF_MAX_FILES    = 300;      // so viele Dateien bekommen einen Zeilen-Diff
const DIFF_TOTAL_BYTES  = 4194304;  // Gesamtbudget der Diffs einer Commit-Seite

function repoHeader(array $repo, string $tab, ?string $ref = null): string
{
    $tabs = ['code' => ['code', 'Code'], 'history' => ['history', 'Historie'], 'branches' => ['branch', 'Branches']];
    if ($repo['is_owner']) {
        $tabs['settings'] = ['gear', 'Einstellungen'];
    }
    ob_start(); ?>
<div class="repo-head">
  <h1 class="repo-title">
    <?= icon('repo') ?>
    <a href="<?= h(webUrl(['view' => $repo['is_owner'] ? 'repos' : 'explore'])) ?>" class="repo-owner"><?= h($repo['owner']) ?></a>
    <span class="repo-sep">/</span>
    <a href="<?= h(repoUrl($repo)) ?>"><strong><?= h($repo['name']) ?></strong></a>
    <?= visibilityBadge($repo) ?>
  </h1>
  <?php if ($repo['description'] !== ''): ?><p class="repo-desc"><?= h($repo['description']) ?></p><?php endif; ?>
  <nav class="tabs" aria-label="Repository">
    <?php foreach ($tabs as $key => [$ic, $label]):
        $q = ($ref !== null && $key !== 'settings' && $key !== 'branches') ? ['ref' => $ref] : []; ?>
    <a href="<?= h(repoUrl($repo, $key, $q)) ?>"<?= $tab === $key ? ' class="active" aria-current="page"' : '' ?>><?= icon($ic) ?><?= h($label) ?></a>
    <?php endforeach; ?>
  </nav>
</div>
<?php
    return (string)ob_get_clean();
}

/* Auswahlfeld Branch/Commit; navigiert per JS, ohne JS per GET-Formular */
function refSelect(array $repo, RepoReader $rd, string $ref, string $view, string $path = ''): string
{
    $branches = $rd->branches();
    $isOid    = RepoReader::isOid($ref);
    ob_start(); ?>
<form method="get" action="/web.php" class="refselect">
  <input type="hidden" name="r" value="<?= h($repo['owner'] . '/' . $repo['name']) ?>">
  <input type="hidden" name="v" value="<?= h($view) ?>">
  <?php if ($path !== ''): ?><input type="hidden" name="p" value="<?= h($path) ?>"><?php endif; ?>
  <?= icon('branch') ?>
  <select name="ref" data-autosubmit aria-label="Branch">
    <?php if ($isOid): ?><option value="<?= h($ref) ?>" selected>Commit <?= h(shortOid($ref)) ?></option><?php endif; ?>
    <?php foreach ($branches as $b): ?>
    <option value="<?= h($b['short']) ?>"<?= !$isOid && $b['short'] === $ref ? ' selected' : '' ?>><?= h($b['short']) ?></option>
    <?php endforeach; ?>
  </select>
  <noscript><button type="submit" class="btn">Zeigen</button></noscript>
</form>
<?php
    return (string)ob_get_clean();
}

function pathCrumbs(array $repo, string $ref, string $path, bool $lastIsFile = false): string
{
    $html  = '<a href="' . h(repoUrl($repo, 'code', ['ref' => $ref])) . '">' . h($repo['name']) . '</a>';
    $parts = $path === '' ? [] : explode('/', $path);
    $cumul = '';
    foreach ($parts as $i => $seg) {
        $cumul .= ($cumul === '' ? '' : '/') . $seg;
        $isLast = $i === count($parts) - 1;
        $html  .= icon('chevron', 'ic crumb-sep');
        $html  .= ($isLast && $lastIsFile)
            ? '<strong>' . h($seg) . '</strong>'
            : '<a href="' . h(repoUrl($repo, 'code', ['ref' => $ref, 'p' => $cumul])) . '">' . h($seg) . '</a>';
    }
    return '<nav class="breadcrumb" aria-label="Pfad">' . $html . '</nav>';
}

function emptyRepo(array $repo): string
{
    $name = $repo['name'];
    ob_start(); ?>
<div class="card empty-repo">
  <?= icon('repo', 'ic empty-ic') ?>
  <strong>Dieses Repository ist noch leer</strong>
  <?php if ($repo['is_owner']): ?>
  <p>Im Arbeitsverzeichnis auf dem Amiga oder PC:</p>
  <pre data-copy-src>amivcs init
amivcs commit -a -m "Erster Stand"
amivcs push <?= h($name) ?></pre>
  <button type="button" class="btn" data-copy><?= icon('copy') ?>Kopieren</button>
  <?php endif; ?>
</div>
<?php
    return (string)ob_get_clean();
}

function commitBar(array $repo, string $oid, array $c): string
{
    return '<div class="commitbar">' . icon('commit')
        . '<a href="' . h(repoUrl($repo, 'commit', ['c' => $oid])) . '" class="commitbar-msg">' . h(subjectLine($c['message'])) . '</a>'
        . '<span class="commitbar-meta">' . h($c['author']) . ' · <span title="' . h(date('d.m.Y H:i', $c['mtime'])) . '">'
        . h(relTime($c['mtime'])) . '</span> · <a class="mono" href="' . h(repoUrl($repo, 'commit', ['c' => $oid])) . '">'
        . h(shortOid($oid)) . '</a></span></div>';
}

/* ── Code (Verzeichnis) ────────────────────────────────────────────── */

function renderCode(array $repo, RepoReader $rd, string $ref, string $commitOid, string $path, array $node): string
{
    $c       = $rd->commit($commitOid);
    $entries = $rd->tree($node['oid']) ?? [];
    $hidden  = max(0, count($entries) - TREE_MAX_ENTRIES);
    $entries = array_slice($entries, 0, TREE_MAX_ENTRIES);
    $rd->prefetchTypes(array_column($entries, 'oid'));
    $readme  = null;
    foreach ($entries as $e) {
        if (!$e['is_dir'] && preg_match('/^read\s?me(\.(txt|md|1st|guide))?$/i', $e['name'])) {
            $readme = $e;
            break;
        }
    }
    ob_start(); ?>
<?= repoHeader($repo, 'code', $ref) ?>
<div class="code-toolbar">
  <?= refSelect($repo, $rd, $ref, 'code', $path) ?>
  <?= pathCrumbs($repo, $ref, $path) ?>
  <div class="code-actions">
    <?php if ($repo['is_owner']): ?>
    <button type="button" class="btn" data-open-dialog="dlgClone"><?= icon('download') ?>Klonen</button>
    <?php endif; ?>
    <a class="btn" href="<?= h(repoUrl($repo, 'zip', ['ref' => $ref, 'p' => $path])) ?>"><?= icon('download') ?>ZIP</a>
  </div>
</div>
<?= commitBar($repo, $commitOid, $c) ?>
<table class="filetable treetable">
  <tbody>
  <?php if ($path !== ''): ?>
    <tr><td class="col-name"><a class="name" href="<?= h(repoUrl($repo, 'code', ['ref' => $ref, 'p' => strpos($path, '/') === false ? '' : dirname($path)])) ?>">
      <span class="kind kind-folder"><?= icon('folder') ?></span><span class="name-text">..</span></a></td><td class="col-size"></td></tr>
  <?php endif; ?>
  <?php foreach ($entries as $e):
      $p = ($path === '' ? '' : $path . '/') . $e['name'];
      $kind = fileKind($e['name'], $e['is_dir']); ?>
    <tr>
      <td class="col-name"><a class="name" href="<?= h(repoUrl($repo, $e['is_dir'] ? 'code' : 'blob', ['ref' => $ref, 'p' => $p])) ?>">
        <span class="kind kind-<?= $kind ?>"><?= icon($kind) ?></span><span class="name-text"><?= h($e['name']) ?></span></a></td>
      <td class="col-size"><?= $e['is_dir'] ? '' : h(fmtSize($rd->blobSize($e['oid']))) ?></td>
    </tr>
  <?php endforeach; ?>
  <?php if ($entries === []): ?>
    <tr class="empty-row"><td colspan="2"><div class="empty"><strong>Leeres Verzeichnis</strong></div></td></tr>
  <?php endif; ?>
  <?php if ($hidden > 0): ?>
    <tr class="empty-row"><td colspan="2"><p class="diff-note"><?= $hidden ?> weitere Einträge nicht angezeigt – bitte als ZIP laden oder klonen.</p></td></tr>
  <?php endif; ?>
  </tbody>
</table>
<?php if ($readme !== null):
    $data = $rd->bytes($readme['oid'], RepoReader::TYPE_BLOB);
    if ($data !== null && !RepoReader::isBinary($data) && strlen($data) <= DISPLAY_MAX_BYTES):
        [$text, ] = RepoReader::toUtf8($data); ?>
<section class="card readme">
  <h2><?= icon('text') ?><?= h($readme['name']) ?></h2>
  <pre><?= h($text) ?></pre>
</section>
<?php endif; endif; ?>

<?php if ($repo['is_owner']): ?>
<dialog id="dlgClone" class="dlg dlg-wide">
  <form method="dialog">
    <h2>Repository klonen</h2>
    <p>In <code>amivcs.cfg</code> (bzw. AMIVCSPrefs) muss dieser Server eingetragen sein:</p>
    <pre data-copy-src>server=<?= h(serverUrl()) . "\n" ?>user=<?= h(Auth::$username) . "\n" ?>pass=…</pre>
    <button type="button" class="btn" data-copy><?= icon('copy') ?>Kopieren</button>
    <p>Dann im Zielverzeichnis:</p>
    <pre data-copy-src>amivcs clone <?= h($repo['name']) ?></pre>
    <button type="button" class="btn" data-copy><?= icon('copy') ?>Kopieren</button>
    <div class="dlg-actions"><button value="ok" class="btn btn-primary">Schließen</button></div>
  </form>
</dialog>
<?php endif;
    return (string)ob_get_clean();
}

/* ── Datei ─────────────────────────────────────────────────────────── */

function imageMime(string $name): ?string
{
    $map = ['png' => 'image/png', 'jpg' => 'image/jpeg', 'jpeg' => 'image/jpeg', 'gif' => 'image/gif'];
    return $map[strtolower(pathinfo($name, PATHINFO_EXTENSION))] ?? null;
}

function renderBlob(array $repo, RepoReader $rd, string $ref, string $commitOid, string $path, array $node): string
{
    $size  = $rd->blobSize($node['oid']);
    $image = imageMime($node['name']);
    $data  = ($size <= DISPLAY_MAX_BYTES && $image === null) ? $rd->bytes($node['oid'], RepoReader::TYPE_BLOB) : null;
    $binary = $data !== null && RepoReader::isBinary($data);
    $text = '';
    $latin1 = false;
    if ($data !== null && !$binary) {
        [$text, $latin1] = RepoReader::toUtf8($data);
    }
    $lines = ($data !== null && !$binary) ? Diff::splitLines($text) : [];
    ob_start(); ?>
<?= repoHeader($repo, 'code', $ref) ?>
<div class="code-toolbar">
  <?= refSelect($repo, $rd, $ref, 'blob', $path) ?>
  <?= pathCrumbs($repo, $ref, $path, true) ?>
</div>
<?= commitBar($repo, $commitOid, $rd->commit($commitOid)) ?>
<section class="filebox">
  <div class="filebox-head">
    <span><?= h(fmtSize($size)) ?><?php if ($lines !== []): ?> · <?= count($lines) ?> Zeilen<?php endif; ?>
      <?php if ($latin1): ?><span class="pill pill-info" title="Kein gültiges UTF-8 – als ISO-8859-1 (Amiga) angezeigt">Latin-1</span><?php endif; ?></span>
    <span class="filebox-actions">
      <a class="btn" href="<?= h(repoUrl($repo, 'raw', ['ref' => $ref, 'p' => $path])) ?>">Roh</a>
      <a class="btn" href="<?= h(repoUrl($repo, 'raw', ['ref' => $ref, 'p' => $path, 'dl' => 1])) ?>"><?= icon('download') ?>Herunterladen</a>
    </span>
  </div>
  <?php if ($image !== null): ?>
    <div class="filebox-image"><img src="<?= h(repoUrl($repo, 'raw', ['ref' => $ref, 'p' => $path])) ?>" alt="<?= h($node['name']) ?>"></div>
  <?php elseif ($data === null): ?>
    <div class="empty"><strong>Datei zu groß für die Anzeige</strong><span>Bitte herunterladen.</span></div>
  <?php elseif ($binary): ?>
    <div class="empty"><strong>Binärdatei</strong><span>Wird nicht angezeigt – bitte herunterladen.</span></div>
  <?php else: ?>
    <div class="code-scroll"><table class="code">
    <?php foreach ($lines as $i => $line): ?>
      <tr id="L<?= $i + 1 ?>"><td class="ln"><a href="#L<?= $i + 1 ?>"><?= $i + 1 ?></a></td><td class="src"><?= h($line) ?></td></tr>
    <?php endforeach; ?>
    </table></div>
  <?php endif; ?>
</section>
<?php
    return (string)ob_get_clean();
}

/* ── Historie ──────────────────────────────────────────────────────── */

/* $topLine: die Spur dieses Commits kommt von oben (sonst Branch-Spitze) */
function graphSvg(array $row, int $maxLanes, bool $topLine): string
{
    /* 37 px = Zeilenhoehe inkl. 1 px Trennlinie, damit Spuren durchgehen */
    $w = max(1, $maxLanes) * 14 + 4;
    $x = function (int $lane): int { return $lane * 14 + 9; };
    $s = '<svg class="graph" width="' . $w . '" height="37" viewBox="0 0 ' . $w . ' 37" aria-hidden="true">';
    foreach ($row['active_lanes'] as $lane) {
        if ($lane !== $row['lane']) {
            $s .= '<line x1="' . $x($lane) . '" y1="0" x2="' . $x($lane) . '" y2="37" class="lane lane' . ($lane % 6) . '"/>';
        }
    }
    if ($topLine) {
        $s .= '<line x1="' . $x($row['lane']) . '" y1="0" x2="' . $x($row['lane']) . '" y2="18" class="lane lane' . ($row['lane'] % 6) . '"/>';
    }
    foreach ($row['parent_lanes'] as $p) {
        $s .= '<path d="M' . $x($row['lane']) . ' 18 C' . $x($row['lane']) . ' 30 ' . $x($p) . ' 26 ' . $x($p) . ' 37" class="lane lane' . ($p % 6) . '"/>';
    }
    $s .= '<circle cx="' . $x($row['lane']) . '" cy="18" r="4.5" class="dot lane' . ($row['lane'] % 6) . '"/>';
    return $s . '</svg>';
}

function renderHistory(array $repo, RepoReader $rd, string $ref, int $page): string
{
    $branches = $rd->branches();
    if ($ref === '') {
        $refs = $branches;
    } else {
        $oid  = $rd->resolve($ref);
        $refs = $oid === null ? [] : [['name' => $ref, 'oid' => $oid]];
    }
    $graph = $rd->history($refs, ($page + 1) * HISTORY_PAGE + 1);
    /* Welche Spuren unten aus der jeweils vorigen Zeile weiterlaufen */
    $open = [];
    foreach ($graph['rows'] as $i => $row) {
        $graph['rows'][$i]['top'] = isset($open[$row['lane']]);
        $open = [];
        foreach ($row['active_lanes'] as $l) {
            if ($l !== $row['lane']) $open[$l] = true;
        }
        foreach ($row['parent_lanes'] as $l) {
            $open[$l] = true;
        }
    }
    $rows  = array_slice($graph['rows'], $page * HISTORY_PAGE, HISTORY_PAGE);
    $more  = count($graph['rows']) > ($page + 1) * HISTORY_PAGE;
    ob_start(); ?>
<?= repoHeader($repo, 'history', $ref === '' ? null : $ref) ?>
<form method="get" action="/web.php" class="refselect">
  <input type="hidden" name="r" value="<?= h($repo['owner'] . '/' . $repo['name']) ?>">
  <input type="hidden" name="v" value="history">
  <?= icon('branch') ?>
  <select name="ref" data-autosubmit aria-label="Branch">
    <option value="">Alle Branches</option>
    <?php foreach ($branches as $b): ?>
    <option value="<?= h($b['short']) ?>"<?= $b['short'] === $ref ? ' selected' : '' ?>><?= h($b['short']) ?></option>
    <?php endforeach; ?>
  </select>
  <noscript><button type="submit" class="btn">Zeigen</button></noscript>
</form>
<?php if ($rows === []): ?>
  <div class="empty card"><?= icon('history', 'ic empty-ic') ?><strong>Noch keine Commits</strong></div>
<?php else: ?>
<table class="historytable">
  <tbody>
  <?php foreach ($rows as $row): ?>
    <tr>
      <td class="col-graph"><?= graphSvg($row, $graph['max_lanes'], $row['top']) ?></td>
      <td class="col-msg">
        <?php foreach (array_filter(explode(', ', $row['decoration'])) as $d): ?><span class="deco"><?= icon('branch') ?><?= h($d) ?></span><?php endforeach; ?>
        <a href="<?= h(repoUrl($repo, 'commit', ['c' => $row['oid']])) ?>"><?= h(subjectLine($row['message'])) ?></a>
        <?php if (count($row['parents']) > 1): ?><span class="pill pill-info">Merge</span><?php endif; ?>
      </td>
      <td class="col-author"><?= h($row['author']) ?></td>
      <td class="col-mod" title="<?= h(date('d.m.Y H:i', $row['mtime'])) ?>"><?= h(relTime($row['mtime'])) ?></td>
      <td class="col-oid"><a class="mono" href="<?= h(repoUrl($repo, 'commit', ['c' => $row['oid']])) ?>"><?= h(shortOid($row['oid'])) ?></a></td>
    </tr>
  <?php endforeach; ?>
  </tbody>
</table>
<nav class="pager">
  <?php if ($page > 0): ?><a class="btn" href="<?= h(repoUrl($repo, 'history', ['ref' => $ref, 'page' => $page - 1])) ?>">Neuere</a><?php endif; ?>
  <?php if ($more): ?><a class="btn" href="<?= h(repoUrl($repo, 'history', ['ref' => $ref, 'page' => $page + 1])) ?>">Ältere</a><?php endif; ?>
</nav>
<?php endif;
    return (string)ob_get_clean();
}

/* ── Commit mit Diff ───────────────────────────────────────────────── */

/* Diff einer Datei: ['binary'|'toolarge'|'hunks', plus, minus, latin1] */
function fileDiff(RepoReader $rd, ?string $old, ?string $new): array
{
    $res = ['kind' => 'hunks', 'hunks' => [], 'plus' => 0, 'minus' => 0, 'latin1' => false];
    $sizes = ($old ? $rd->blobSize($old) : 0) + ($new ? $rd->blobSize($new) : 0);
    if ($sizes > 2 * DIFF_MAX_BYTES) {
        $res['kind'] = 'toolarge';
        return $res;
    }
    $a = $old ? ($rd->bytes($old, RepoReader::TYPE_BLOB) ?? '') : '';
    $b = $new ? ($rd->bytes($new, RepoReader::TYPE_BLOB) ?? '') : '';
    if (RepoReader::isBinary($a) || RepoReader::isBinary($b)) {
        $res['kind'] = 'binary';
        return $res;
    }
    [$a, $la] = RepoReader::toUtf8($a);
    [$b, $lb] = RepoReader::toUtf8($b);
    $res['latin1'] = $la || $lb;
    $la = Diff::splitLines($a);
    $lb = Diff::splitLines($b);
    if (count($la) > DIFF_MAX_LINES || count($lb) > DIFF_MAX_LINES) {
        $res['kind'] = 'toolarge';
        return $res;
    }
    $ops = Diff::ops($la, $lb);
    foreach ($ops as $op) {
        if ($op[0] === '+') $res['plus']++;
        if ($op[0] === '-') $res['minus']++;
    }
    $res['hunks'] = Diff::hunks($ops);
    return $res;
}

function renderCommit(array $repo, RepoReader $rd, string $oid): string
{
    $c       = $rd->commit($oid);
    $files   = $rd->changedFiles($oid);
    $diffs   = [];
    $plus    = $minus = 0;
    $budget  = DIFF_TOTAL_BYTES;
    foreach ($files as $i => $f) {
        $size = ($f['old'] ? $rd->blobSize($f['old']) : 0) + ($f['new'] ? $rd->blobSize($f['new']) : 0);
        if ($i >= DIFF_MAX_FILES || $size > $budget) {
            /* Budget erschoepft: nur noch die Dateiliste */
            $diffs[$i] = ['kind' => 'skipped', 'hunks' => [], 'plus' => 0, 'minus' => 0, 'latin1' => false];
            continue;
        }
        $budget   -= $size;
        $diffs[$i] = fileDiff($rd, $f['old'], $f['new']);
        $plus  += $diffs[$i]['plus'];
        $minus += $diffs[$i]['minus'];
    }
    $msg  = rtrim($c['message']);
    $subj = subjectLine($msg);
    $body = trim(substr($msg, strlen($subj)));
    $statusLabel = ['A' => 'neu', 'D' => 'gelöscht', 'M' => 'geändert'];
    ob_start(); ?>
<?= repoHeader($repo, 'history') ?>
<section class="card commit-head">
  <h2><?= h($subj) ?></h2>
  <?php if ($body !== ''): ?><pre class="commit-body"><?= h($body) ?></pre><?php endif; ?>
  <dl class="meta commit-meta">
    <dt>Autor</dt><dd><?= h($c['author']) ?></dd>
    <dt>Datum</dt><dd><?= h(date('d.m.Y H:i', $c['mtime'])) ?> (<?= h(relTime($c['mtime'])) ?>)</dd>
    <dt>Commit</dt><dd class="mono"><?= h($oid) ?></dd>
    <dt><?= count($c['parents']) > 1 ? 'Eltern' : 'Elternteil' ?></dt>
    <dd><?php foreach ($c['parents'] as $p): ?><a class="mono" href="<?= h(repoUrl($repo, 'commit', ['c' => $p])) ?>"><?= h(shortOid($p)) ?></a> <?php endforeach; ?>
      <?php if ($c['parents'] === []): ?>– (erster Commit)<?php endif; ?></dd>
  </dl>
  <p class="commit-actions">
    <a class="btn" href="<?= h(repoUrl($repo, 'code', ['ref' => $oid])) ?>"><?= icon('code') ?>Dateien zu diesem Stand</a>
    <?php if (count($c['parents']) > 1): ?><span class="muted">Merge-Commit: Änderungen gegenüber dem ersten Elternteil.</span><?php endif; ?>
  </p>
</section>

<p class="diffstat"><?= count($files) ?> <?= count($files) === 1 ? 'Datei' : 'Dateien' ?> geändert ·
  <span class="plus">+<?= $plus ?></span> <span class="minus">−<?= $minus ?></span></p>
<?php if ($rd->truncated): ?>
<p class="diff-note">Dieser Commit ist zu groß für die vollständige Anzeige – die Liste ist gekürzt.</p>
<?php endif; ?>

<?php foreach ($files as $i => $f):
    $d = $diffs[$i];
    $big = count($d['hunks']) > 0 && array_sum(array_map(function ($h) { return count($h['lines']); }, $d['hunks'])) > 400; ?>
<details class="diff"<?= $big ? '' : ' open' ?> id="f<?= $i ?>">
  <summary>
    <span class="pill pill-<?= $f['status'] ?>"><?= h($statusLabel[$f['status']]) ?></span>
    <span class="diff-path"><?= h($f['path']) ?></span>
    <span class="diff-count"><span class="plus">+<?= $d['plus'] ?></span> <span class="minus">−<?= $d['minus'] ?></span></span>
    <?php if ($d['latin1']): ?><span class="pill pill-info">Latin-1</span><?php endif; ?>
    <?php if ($f['new'] !== null): ?>
    <a class="diff-view" href="<?= h(repoUrl($repo, 'blob', ['ref' => $oid, 'p' => $f['path']])) ?>">Ansehen</a>
    <?php endif; ?>
  </summary>
  <?php if ($d['kind'] === 'binary'): ?>
    <p class="diff-note">Binärdatei <?= $f['status'] === 'M' ? 'geändert' : ($f['status'] === 'A' ? 'hinzugefügt' : 'gelöscht') ?>.</p>
  <?php elseif ($d['kind'] === 'toolarge'): ?>
    <p class="diff-note">Diff zu groß für die Anzeige.</p>
  <?php elseif ($d['kind'] === 'skipped'): ?>
    <p class="diff-note">Diff nicht angezeigt – zu viele Änderungen in diesem Commit.</p>
  <?php elseif ($d['hunks'] === []): ?>
    <p class="diff-note">Leere Datei.</p>
  <?php else: ?>
  <div class="code-scroll"><table class="code diffcode">
    <?php foreach ($d['hunks'] as $hk): ?>
    <tr class="hunk"><td class="ln"></td><td class="ln"></td><td class="src">@@ −<?= $hk['old_start'] ?>,<?= $hk['old_len'] ?> +<?= $hk['new_start'] ?>,<?= $hk['new_len'] ?> @@</td></tr>
      <?php foreach ($hk['lines'] as $op):
          $cls = $op[0] === '+' ? 'add' : ($op[0] === '-' ? 'del' : ''); ?>
    <tr class="<?= $cls ?>"><td class="ln"><?= $op[1] ?: '' ?></td><td class="ln"><?= $op[2] ?: '' ?></td><td class="src"><span class="sign"><?= $op[0] === ' ' ? ' ' : ($op[0] === '-' ? '−' : '+') ?></span><?= h($op[3]) ?></td></tr>
      <?php endforeach; ?>
    <?php endforeach; ?>
  </table></div>
  <?php endif; ?>
</details>
<?php endforeach; ?>
<?php if ($files === []): ?>
  <div class="empty card"><strong>Keine Dateiänderungen</strong></div>
<?php endif;
    return (string)ob_get_clean();
}

/* ── Branches ──────────────────────────────────────────────────────── */

function renderBranches(array $repo, RepoReader $rd): string
{
    $branches = $rd->branches();
    $def = $rd->defaultBranch();
    ob_start(); ?>
<?= repoHeader($repo, 'branches') ?>
<?php if ($branches === []): ?>
  <div class="empty card"><?= icon('branch', 'ic empty-ic') ?><strong>Noch keine Branches</strong></div>
<?php else: ?>
<div class="table-scroll">
<table class="datatable">
  <thead><tr><th>Branch</th><th>Letzter Commit</th><th>Autor</th><th>Wann</th></tr></thead>
  <tbody>
  <?php foreach ($branches as $b):
      $c = $rd->commit($b['oid']); ?>
    <tr>
      <td><a href="<?= h(repoUrl($repo, 'code', ['ref' => $b['short']])) ?>"><?= icon('branch') ?> <strong><?= h($b['short']) ?></strong></a>
        <?php if ($def !== null && $def['name'] === $b['name']): ?><span class="pill pill-public">Standard</span><?php endif; ?></td>
      <td><?php if ($c): ?><a href="<?= h(repoUrl($repo, 'commit', ['c' => $b['oid']])) ?>"><?= h(subjectLine($c['message'])) ?></a> <span class="mono muted"><?= h(shortOid($b['oid'])) ?></span><?php endif; ?></td>
      <td><?= $c ? h($c['author']) : '' ?></td>
      <td><?= $c ? h(relTime($c['mtime'])) : '' ?></td>
    </tr>
  <?php endforeach; ?>
  </tbody>
</table>
</div>
<?php endif;
    return (string)ob_get_clean();
}

/* ── Einstellungen (Eigentuemer) ───────────────────────────────────── */

function renderRepoSettings(array $repo, RepoReader $rd): string
{
    $branches = $rd->branches();
    ob_start(); ?>
<?= repoHeader($repo, 'settings') ?>
<div class="settings">
  <section class="card card-wide">
    <h2>Allgemein</h2>
    <form method="post" action="/web.php" class="form-grid">
      <?= csrfField() ?><input type="hidden" name="do" value="repo_settings">
      <input type="hidden" name="r" value="<?= h($repo['owner'] . '/' . $repo['name']) ?>">
      <label for="rsDesc">Beschreibung</label>
      <input id="rsDesc" name="description" maxlength="255" value="<?= h($repo['description']) ?>">
      <label for="rsBranch">Standard-Branch</label>
      <select id="rsBranch" name="default_branch">
        <?php if ($branches === []): ?><option value="<?= h($repo['default_branch']) ?>"><?= h(preg_replace('#^refs/heads/#', '', $repo['default_branch'])) ?></option><?php endif; ?>
        <?php foreach ($branches as $b): ?>
        <option value="<?= h($b['name']) ?>"<?= $b['name'] === $repo['default_branch'] ? ' selected' : '' ?>><?= h($b['short']) ?></option>
        <?php endforeach; ?>
      </select>
      <span></span><label class="check"><input type="checkbox" name="is_public" value="1"<?= (int)$repo['is_public'] ? ' checked' : '' ?>> Öffentlich – jeder kann es im Browser ansehen und als ZIP laden</label>
      <p class="form-hint">Push, Pull und Klonen bleiben immer Ihnen vorbehalten.</p>
      <div class="form-actions"><button type="submit" class="btn btn-primary">Speichern</button></div>
    </form>
  </section>
  <section class="card card-wide danger-zone">
    <h2>Repository löschen</h2>
    <p class="form-hint">Löscht alle Commits, Branches und Dateien auf dem Server endgültig. Lokale Kopien auf Ihren Rechnern bleiben erhalten.</p>
    <form method="post" action="/web.php" class="inline-form">
      <?= csrfField() ?><input type="hidden" name="do" value="repo_delete">
      <input type="hidden" name="r" value="<?= h($repo['owner'] . '/' . $repo['name']) ?>">
      <input name="confirm" required autocomplete="off" autocapitalize="none" spellcheck="false"
             placeholder="Zur Bestätigung „<?= h($repo['name']) ?>“ eintippen" aria-label="Repository-Name zur Bestätigung">
      <button type="submit" class="btn btn-danger"><?= icon('trash') ?>Endgültig löschen</button>
    </form>
  </section>
</div>
<?php
    return (string)ob_get_clean();
}
