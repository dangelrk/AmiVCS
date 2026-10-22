/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * Weboberflaeche: Menues, Dialoge, Kopieren, Bestaetigungen, Benutzer-
 * Dialoge, Branch-Auswahl. Alles funktioniert auch ohne JavaScript
 * (Formulare), das hier ist nur Komfort. Grundstock aus AmiCloud. */
(function () {
  'use strict';

  var $  = function (sel, root) { return (root || document).querySelector(sel); };
  var $$ = function (sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); };

  /* ── Menues ──────────────────────────────────────────────────────── */
  function closeMenus(except) {
    $$('.menu').forEach(function (m) { if (m !== except) m.hidden = true; });
    $$('[data-menu]').forEach(function (b) { b.setAttribute('aria-expanded', 'false'); });
  }
  document.addEventListener('click', function (ev) {
    var btn = ev.target.closest('[data-menu]');
    if (btn) {
      var menu = document.getElementById(btn.dataset.menu);
      var open = menu.hidden;
      closeMenus(menu);
      menu.hidden = !open;
      btn.setAttribute('aria-expanded', open ? 'true' : 'false');
      ev.stopPropagation();
      return;
    }
    if (!ev.target.closest('.menu')) closeMenus();
  });
  document.addEventListener('keydown', function (ev) {
    if (ev.key === 'Escape') closeMenus();
  });

  $$('[data-toggle-nav]').forEach(function (b) {
    b.addEventListener('click', function () { document.body.classList.toggle('nav-open'); });
  });
  $$('[data-dismiss]').forEach(function (b) {
    b.addEventListener('click', function () { b.parentNode.remove(); });
  });
  $$('form[data-confirm]').forEach(function (f) {
    f.addEventListener('submit', function (ev) { if (!confirm(f.dataset.confirm)) ev.preventDefault(); });
  });

  /* Branch-/Commit-Auswahl wechselt sofort */
  $$('select[data-autosubmit]').forEach(function (s) {
    s.addEventListener('change', function () { s.form.submit(); });
  });

  /* ── Kopieren: naechstes [data-copy-src] im selben Elternelement ─── */
  document.addEventListener('click', function (ev) {
    var b = ev.target.closest('[data-copy]');
    if (!b) return;
    var src = b.parentNode.querySelector('[data-copy-src]');
    if (!src) return;
    var done = function () { b.classList.add('copied'); setTimeout(function () { b.classList.remove('copied'); }, 1200); };
    var select = function () {
      var r = document.createRange(); r.selectNodeContents(src);
      var s = window.getSelection(); s.removeAllRanges(); s.addRange(r);
    };
    if (navigator.clipboard) {
      navigator.clipboard.writeText(src.textContent.trim()).then(done, select);
    } else {
      select();
    }
  });

  /* ── Dialoge ─────────────────────────────────────────────────────── */
  $$('[data-open-dialog]').forEach(function (b) {
    b.addEventListener('click', function () { document.getElementById(b.dataset.openDialog).showModal(); });
  });
  document.addEventListener('click', function (ev) {
    var b = ev.target.closest('[data-close-dialog]');
    if (b) b.closest('dialog').close();
  });

  /* ── Benutzerverwaltung ──────────────────────────────────────────── */
  var dlgEdit = document.getElementById('dlgUserEdit');
  if (dlgEdit) {
    $$('[data-edit-user]').forEach(function (b) {
      b.addEventListener('click', function () {
        var u = JSON.parse(b.dataset.editUser);
        var f = $('form', dlgEdit);
        f.id.value = u.id;
        f.display_name.value = u.display_name;
        f.email.value = u.email;
        f.password.value = '';
        f.is_admin.checked = u.is_admin;
        f.active.checked = u.active;
        $('[data-username]', dlgEdit).textContent = u.username;
        dlgEdit.showModal();
      });
    });
    var dlgDel = document.getElementById('dlgUserDelete');
    $$('[data-delete-user]').forEach(function (b) {
      b.addEventListener('click', function () {
        var u = JSON.parse(b.dataset.deleteUser);
        var f = $('form', dlgDel);
        f.id.value = u.id;
        f.confirm.value = '';
        $('[data-username]', dlgDel).textContent = u.username;
        dlgDel.showModal();
      });
    });
  }

  /* Zeilenanker in der Dateiansicht hervorheben */
  function markLine() {
    $$('tr.hl').forEach(function (r) { r.classList.remove('hl'); });
    var m = /^#L(\d+)$/.exec(location.hash);
    if (m) {
      var row = document.getElementById('L' + m[1]);
      if (row) row.classList.add('hl');
    }
  }
  window.addEventListener('hashchange', markLine);
  markLine();
})();
