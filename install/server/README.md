# AmiVCS-Server @VERSION@

Server für AmiVCS: Objekt- und Ref-Speicher für `clone`, `push` und `pull`
sowie eine Weboberfläche zum Ansehen der Repositories. Läuft mit Apache,
PHP (ab 7.4) und MariaDB oder MySQL. Er ist unabhängig von einem
AmiCloud-Server auf derselben Maschine (eigene Datenbank, eigene Konten).

## Inhalt des Archivs

| Pfad | Inhalt |
|---|---|
| `server/` | Dateien für das Web-Verzeichnis des Servers |
| `LICENSE` | GPL-3.0-or-later |

Alle Befehle unten werden in diesem Ordner ausgeführt (dort, wo `server/`
liegt). `config.php` ist nicht im Archiv, sie wird aus
`server/config.php.example` angelegt.

## Neu installieren

Die Schritte im Abschnitt „AmiVCS-Server einrichten“ weiter unten.

## Aktualisieren

1. **Sichern** (Pfade wie bei der Installation):
   ```sh
   sudo tar czf /root/amivcs-backup-$(date +%Y%m%d).tgz -C /var/www amivcs
   sudo mysqldump amivcs > /root/amivcs-db-$(date +%Y%m%d).sql
   ```
2. **Datenbank nachrüsten**, und zwar *vor* dem Kopieren der Dateien. Die
   noch fehlenden Dateien `server/sql/migrate_00x.sql` der Reihe nach als
   MariaDB-root einspielen:
   - Von 0.1 oder neuer auf @VERSION@: keine Migration nötig.
   - Installationen von vor 0.1 brauchen `migrate_001.sql` und
     `migrate_002.sql`.
3. **Dateien kopieren.** Das Archiv enthält keine `config.php`, die
   vorhandene bleibt also erhalten. `server/.` mit Punkt, damit die
   `.htaccess`-Dateien mitkommen:
   ```sh
   sudo cp -r server/. /var/www/amivcs/
   sudo chown -R root:apache /var/www/amivcs
   sudo chmod -R u=rwX,g=rX,o= /var/www/amivcs
   sudo chmod 640 /var/www/amivcs/config.php
   ```
4. **Testen:**
   ```sh
   curl -u bob:geheim "https://amivcs.example.com/api.php?action=status"
   ```

Die Versionsnummer in der Antwort von `status` (`"version":"0.1"`) ist die
Version der Server-Schnittstelle, nicht die der Programme.
