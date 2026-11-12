# AmiVCS-Server auf @VERSION@ aktualisieren

Diese Anleitung ist für einen **schon laufenden** AmiVCS-Server. Für eine
Neuinstallation gilt `INSTALL.md`; dort legt `schema.sql` die Datenbank
gleich vollständig an, Migrationen braucht es dann nicht.

Die Befehle gehen von den Pfaden aus `INSTALL.md` aus (`/var/www/amivcs`,
Datenbank `amivcs`, Webserver-Gruppe `apache`) und werden in diesem Ordner
ausgeführt, also dort, wo `server/` liegt.

## Welche Migrationen brauche ich?

Eine Migration ändert die Datenbank. Jede wird **genau einmal** eingespielt,
in der Reihenfolge ihrer Nummer, **vor** dem Kopieren der neuen Dateien.

| Server bisher | einspielen |
|---|---|
| 0.1 bis @VERSION@ | nichts |
| älter als 0.1 | `migrate_001.sql`, `migrate_002.sql` |

Ob eine Migration schon eingespielt ist, zeigt die Abfrage oben in der
jeweiligen Datei:

| Migration | ist eingespielt, wenn |
|---|---|
| `migrate_001.sql` | `SHOW COLUMNS FROM users LIKE 'is_admin';` eine Zeile liefert |
| `migrate_002.sql` | `SHOW CREATE TABLE refs;` `name` als `varbinary(191)` zeigt |

Eine Migration lässt sich nicht zweimal einspielen.

## Schritt für Schritt

1. **Sichern** – Dateien und Datenbank:
   ```sh
   sudo tar czf /root/amivcs-backup-$(date +%Y%m%d).tgz -C /var/www amivcs
   sudo mysqldump amivcs > /root/amivcs-db-$(date +%Y%m%d).sql
   ```
2. **Datenbank nachrüsten** – nur, wenn die Tabelle oben eine Migration
   nennt, als MariaDB-root:
   ```sh
   sudo mysql amivcs < server/sql/migrate_001.sql
   sudo mysql amivcs < server/sql/migrate_002.sql
   ```
3. **Dateien kopieren.** Das Archiv enthält keine `config.php`, die
   vorhandene bleibt erhalten. `server/.` mit Punkt, damit die
   `.htaccess`-Dateien mitkommen:
   ```sh
   sudo cp -r server/. /var/www/amivcs/
   sudo chown -R root:apache /var/www/amivcs
   sudo chmod -R u=rwX,g=rX,o= /var/www/amivcs
   sudo chmod 640 /var/www/amivcs/config.php
   ```
4. **Prüfen:**
   ```sh
   curl -u bob:geheim "https://amivcs.example.com/api.php?action=status"
   ```

## Was sich mit @VERSION@ ändert

Am Server nichts, was eine Migration braucht. Neu ist eine Regel in den
**Programmen**: `commit` nimmt ohne weitere Einstellung keine neuen Namen
mehr an, die ein Amiga mit klassischem FFS nicht speichern kann (mehr als
30 Zeichen, `:` oder nur in Groß-/Kleinschreibung verschieden). Pro
Repository lässt sich das mit `amivcs config longnames on` und einer
committeten `.amivcsproject` aufheben. Der Server speichert solche
Repositories wie bisher.

## Zurück zur alten Fassung

```sh
sudo tar xzf /root/amivcs-backup-JJJJMMTT.tgz -C /var/www
sudo mysql amivcs < /root/amivcs-db-JJJJMMTT.sql
```
