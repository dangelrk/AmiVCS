# AmiVCS-Server @VERSION@

Server für AmiVCS: Objekt- und Ref-Speicher für `clone`, `push` und `pull`
sowie eine Weboberfläche zum Ansehen der Repositories. Läuft mit Apache,
PHP (ab 7.4) und MariaDB oder MySQL. Er ist unabhängig von einem
AmiCloud-Server auf derselben Maschine (eigene Datenbank, eigene Konten).

## Inhalt des Archivs

| Pfad | Inhalt |
|---|---|
| `server/` | Dateien für das Web-Verzeichnis des Servers |
| `UPDATE.md` | eine bestehende Installation aktualisieren (Datenbank-Migrationen) |
| `LICENSE` | GPL-3.0-or-later |

Alle Befehle unten werden in diesem Ordner ausgeführt (dort, wo `server/`
liegt). `config.php` ist nicht im Archiv, sie wird aus
`server/config.php.example` angelegt.

## Neu installieren

Die Schritte im Abschnitt „AmiVCS-Server einrichten“ weiter unten.

## Aktualisieren

Eine schon laufende Installation aktualisiert man nach **`UPDATE.md`** in
diesem Ordner: sichern, die nötigen Datenbank-Migrationen einspielen (eine
Tabelle dort sagt, welche), Dateien kopieren, prüfen – und was sich mit
dieser Version am Verhalten ändert.

Die Versionsnummer in der Antwort von `status` (`"version":"0.1"`) ist die
Version der Server-Schnittstelle, nicht die der Programme.
