# ADR 10: Weboberfläche mit lesender Repository-Anzeige und öffentlichen Repositories

Stand: 22.10.2026 · ergänzt [ADR 09](09_ADR_VCS_Design.md) ·
Plan: [docs/design/webui-plan.md](../design/webui-plan.md)

## Kontext

ADR 09 legt fest, dass der Server „dumm“ bleibt: Er speichert Objekte und
Branch-Zeiger, jede VCS-Entscheidung (Merge, Ancestry) fällt auf dem Client.
Die erste Web-Oberfläche hat dafür eine schmale Ausnahme gemacht und
Commit-Objekte nur zur Anzeige der Historie dekodiert. Gewünscht ist nun
eine Oberfläche im Stil von Gitea: Dateien, Commits und Diffs ansehen,
Repositories öffentlich zeigen, Benutzer im Browser verwalten.

## Entscheidung

1. **Lesende Anzeige ist erlaubt, Entscheidungen bleiben beim Client.**
   Der Server dekodiert Tree-, Commit- und Blob-Inhalte, vergleicht Trees
   (geänderte Dateien), berechnet Zeilen-Diffs und packt ZIP-Dateien – nur
   für die Anzeige. Er merged nicht, ändert keine Branches (außer über das
   bestehende `update-ref` der API) und erzeugt keine Commits. Dateien im
   Browser bearbeiten gibt es nicht.
2. **Ein Eigentümer je Repository** (wie ADR 09). Keine Mitarbeiter,
   keine Teams, keine Forks.
3. **Sichtbarkeit privat/öffentlich** pro Repository (Standard privat).
   Öffentliche Repositories sieht jeder im Browser, auch ohne Konto.
   `clone`/`push`/`pull` über die API bleiben dem Eigentümer vorbehalten.
   Im Web heißen Repositories `<besitzer>/<repo>`; eine einzige Funktion
   (`Repos::forView`) entscheidet über den Zugriff, private und nicht
   vorhandene Repositories sind nicht unterscheidbar (404).
4. **Benutzerverwaltung, App-Passwörter, Login-Bremse** wie bei AmiCloud
   (eigene Datenbank, eigene Konten; der Code ist bewusst dupliziert,
   weil die beiden Server getrennte Produkte ohne gemeinsame
   PHP-Bibliothek sind).
5. **Zeichensatz:** Inhalte, die kein gültiges UTF-8 sind, werden als
   ISO-8859-1 (Amiga) angezeigt; Downloads liefern die Originalbytes.
   Hochgeladene Inhalte werden nie als HTML/SVG unter der eigenen Origin
   ausgeliefert (nur Text als `text/plain` und PNG/JPEG/GIF inline, sonst
   Download, immer mit `nosniff` und `CSP: sandbox`).

## Folgen

- Schema-Erweiterung `sql/migrate_001.sql` (additiv); die API bleibt
  abwärtskompatibel (zusätzlich App-Passwörter, 429, `pushed_at`).
- Die PHP-Decoder (`ObjectDecoder`, `GraphLayout`) müssen mit dem
  Objektformat in `libcloudvcs/src/object.c` synchron bleiben.
- Große Historien/Diffs werden begrenzt (50 Commits je Seite, Diffs bis
  512 KB bzw. 5000 Zeilen), damit der Server nicht unnötig rechnet.
