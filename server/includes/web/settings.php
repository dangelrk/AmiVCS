<?php
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ronny Krueger
if (!defined('AMIVCS_SERVER')) { http_response_code(403); exit; }

/* Persoenliche Einstellungen: Profil, Passwort, Geraete/App-Passwoerter,
 * Client-Einrichtung (aus AmiCloud uebernommen). $newToken ist ein gerade
 * angelegtes App-Passwort, das genau einmal angezeigt wird. */
function renderSettings(?array $newToken): string
{
    $u      = Auth::$user;
    $tokens = AppPasswords::listFor(Auth::$user_id);
    $server = serverUrl();
    ob_start();
?>
<h1 class="view-title"><?= icon('gear') ?>Persönliche Einstellungen</h1>

<div class="settings">
  <section class="card">
    <h2>Profil</h2>
    <form method="post" action="/web.php" class="form-grid">
      <?= csrfField() ?><input type="hidden" name="do" value="profile">
      <label for="pUser">Benutzername</label>
      <input id="pUser" type="text" value="<?= h(Auth::$username) ?>" disabled>
      <label for="pName">Anzeigename</label>
      <input id="pName" type="text" name="display_name" maxlength="128" value="<?= h($u['display_name']) ?>" placeholder="<?= h(Auth::$username) ?>">
      <label for="pMail">E-Mail</label>
      <input id="pMail" type="email" name="email" maxlength="255" value="<?= h($u['email']) ?>">
      <div class="form-actions"><button type="submit" class="btn btn-primary">Speichern</button></div>
    </form>
  </section>

  <section class="card">
    <h2>Passwort</h2>
    <form method="post" action="/web.php" class="form-grid">
      <?= csrfField() ?><input type="hidden" name="do" value="password">
      <label for="pwOld">Aktuelles Passwort</label>
      <input id="pwOld" type="password" name="current" autocomplete="current-password" required>
      <label for="pwNew">Neues Passwort</label>
      <input id="pwNew" type="password" name="new" autocomplete="new-password" minlength="8" required>
      <label for="pwNew2">Wiederholen</label>
      <input id="pwNew2" type="password" name="new2" autocomplete="new-password" minlength="8" required>
      <span></span><label class="check"><input type="checkbox" name="revoke_tokens" value="1" checked> Alle App-Passwörter widerrufen</label>
      <p class="form-hint">Andere angemeldete Browser werden abgemeldet. Widerrufene Geräte brauchen danach ein neues App-Passwort.</p>
      <div class="form-actions"><button type="submit" class="btn btn-primary">Passwort ändern</button></div>
    </form>
  </section>

  <section class="card card-wide" id="devices">
    <h2>Geräte &amp; App-Passwörter</h2>
    <p class="form-hint">Für jeden Amiga oder PC ein eigenes Passwort anlegen und in <code>amivcs.cfg</code>
       bzw. AMIVCSPrefs statt des Kontopassworts eintragen. Geht ein Gerät verloren, widerrufen Sie nur dessen Passwort.</p>

    <?php if ($newToken !== null): ?>
    <div class="newtoken">
      <strong>Neues App-Passwort für „<?= h($newToken['name']) ?>“</strong>
      <p>Es wird nur jetzt angezeigt. Für <code>amivcs.cfg</code>:</p>
      <pre data-copy-src>server=<?= h($server) . "\n" ?>user=<?= h(Auth::$username) . "\n" ?>pass=<?= h($newToken['token']) ?></pre>
      <button type="button" class="btn" data-copy><?= icon('copy') ?>Kopieren</button>
    </div>
    <?php endif; ?>

    <form method="post" action="/web.php" class="inline-form">
      <?= csrfField() ?><input type="hidden" name="do" value="apppw_create">
      <input type="text" name="name" maxlength="64" placeholder="Gerätename, z. B. A1200" required aria-label="Gerätename">
      <input type="password" name="current" placeholder="Ihr Kontopasswort" required autocomplete="current-password" aria-label="Kontopasswort zur Bestätigung">
      <button type="submit" class="btn btn-primary"><?= icon('key') ?>App-Passwort erstellen</button>
    </form>

    <?php if ($tokens !== []): ?>
    <div class="table-scroll">
    <table class="datatable">
      <thead><tr><th>Gerät</th><th>Erstellt</th><th>Zuletzt benutzt</th><th></th></tr></thead>
      <tbody>
      <?php foreach ($tokens as $t): ?>
        <tr>
          <td><?= icon('key') ?> <?= h($t['name']) ?></td>
          <td><?= h(date('d.m.Y', strtotime($t['created_at']))) ?></td>
          <td><?= $t['last_used_at'] ? h(relTime(strtotime($t['last_used_at']))) : 'nie' ?></td>
          <td class="col-right">
            <form method="post" action="/web.php" data-confirm="App-Passwort „<?= h($t['name']) ?>“ widerrufen? Das Gerät kann sich danach nicht mehr anmelden.">
              <?= csrfField() ?><input type="hidden" name="do" value="apppw_revoke"><input type="hidden" name="id" value="<?= (int)$t['id'] ?>">
              <button type="submit" class="btn btn-danger-ghost">Widerrufen</button>
            </form>
          </td>
        </tr>
      <?php endforeach; ?>
      </tbody>
    </table>
    </div>
    <?php endif; ?>
  </section>

  <section class="card">
    <h2>Clients einrichten</h2>
    <p class="form-hint">Server-Adresse für AMIVCSPrefs, <code>amivcs</code> und <code>amivcs-gui</code>:</p>
    <pre data-copy-src><?= h($server) ?></pre>
    <button type="button" class="btn" data-copy><?= icon('copy') ?>Kopieren</button>
  </section>
</div>
<?php
    return (string)ob_get_clean();
}
