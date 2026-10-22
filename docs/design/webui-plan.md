# Plan: AmiVCS-Weboberfläche unter amivcs.planet-rk.de

Stand: 22.10.2026 · Status: **umgesetzt und deployt (22.10.2026)**

## 1. Ziel und Entscheidungen

AmiVCS bekommt eine Weboberfläche im Stil von Gitea/GitHub, passend zur
AmiCloud-Oberfläche (gleiches Farbschema, gleiches Bedienkonzept, eigenes Logo).

Vom Nutzer entschieden (22.10.2026):

| Frage | Entscheidung |
|---|---|
| Sichtbarkeit | **privat oder öffentlich**, pro Repository umschaltbar. Öffentliche Repos sieht jeder im Browser (auch ohne Konto) und kann sie als ZIP laden. |
| Mitarbeiter | **nein** – jedes Repository hat genau einen Eigentümer (ADR 09 bleibt). `clone/push/pull` über die API weiterhin nur für den Eigentümer. |
| Code-Ansicht | **Dateien, Commits, Diffs**: Dateibaum je Branch/Commit, Dateiansicht, Historie mit Graph, Commit-Seite mit geänderten Dateien und zeilenweisem Diff, ZIP-Download. |

Nicht Bestandteil: Dateien im Browser bearbeiten oder hochladen (der Server
bleibt „dumm“, Commits entstehen nur auf dem Client), Issues, Pull-Requests,
Wiki, Mitarbeiter/Teams, Forks, Merge im Browser, Tags.

## 2. Ist-Zustand

- **Produktion:** Auf `amivcs.planet-rk.de` läuft nur die API vom 12.07. (`api.php`,
  `includes/` ohne Web-Teil). `/` liefert 403, `/web.php` 404.
- **Repo:** `server/web.php` (330 Zeilen) kann anmelden, die eigenen Repos
  auflisten und die Historie aller Branches mit Graph zeigen
  (`ObjectDecoder.php`, `GraphLayout.php`). Nie deployt.
- **Datenmodell:** `users` (ohne Admin/Profil), `repos` (owner_id, name),
  `objects` (Metadaten; Bytes auf der Platte unter
  `STORAGE/<owner>/<repo>/objects/xx/…`), `refs`.
- **Auth:** Basic Auth mit bcrypt bei jedem Request, keine Login-Bremse,
  Benutzer per SQL-`INSERT`.
- **Objektformat** (libcloudvcs/object.c): Blob = rohe Dateibytes, OID = MD5;
  Tree = Version, Varint-Anzahl, je Eintrag Flag Verzeichnis, Namenslänge,
  Name, 16 Byte OID; Commit = Tree, Eltern, mtime, Autor, Nachricht.

## 3. Datenmodell (`sql/migrate_001.sql`, additiv; `schema.sql` angepasst)

```sql
ALTER TABLE users
  ADD display_name VARCHAR(128) NOT NULL DEFAULT '',
  ADD email        VARCHAR(255) NOT NULL DEFAULT '',
  ADD is_admin     TINYINT(1)   NOT NULL DEFAULT 0,
  ADD last_login   DATETIME NULL,
  ADD session_epoch INT UNSIGNED NOT NULL DEFAULT 0;

ALTER TABLE repos
  ADD description    VARCHAR(255) NOT NULL DEFAULT '',
  ADD is_public      TINYINT(1)   NOT NULL DEFAULT 0,
  ADD default_branch VARCHAR(191) NOT NULL DEFAULT 'refs/heads/main',
  ADD pushed_at      DATETIME NULL;          -- von update-ref gesetzt

CREATE TABLE app_passwords (… wie AmiCloud …);
CREATE TABLE auth_failures (… wie AmiCloud …);
```

Neue Repos sind **privat**. Bestehende Repos bleiben privat.

## 4. Server-Code

### 4.1 Aus AmiCloud übernommen (angepasst an AMIVCS-Namen/Konstanten)

`Users.php`, `AppPasswords.php`, `Throttle.php`, `Auth.php` (Basic Auth mit
App-Passwort + 429, Web-Session mit `session_epoch`), `bin/amivcs-admin.php`
(`user:add/passwd/admin/disable/enable/delete/list`, zusätzlich `repo:list`),
Web-Gerüst (`includes/web/helpers.php`, `layout.php`), `web.css`/`web.js` im
AmiCloud-Stil. Die Doppelung zwischen beiden Servern ist bewusst: getrennte
Produkte, eigene Datenbanken, keine gemeinsame PHP-Bibliothek
(AmiCloudCore ist C). Kein Speicherkontingent für AmiVCS.

### 4.2 Neu: lesender Repository-Zugriff `includes/RepoReader.php`

Reine Anzeige-Logik, baut auf `ObjectDecoder` auf (Erweiterung der in ADR 09
dokumentierten Ausnahme, festgehalten in **ADR 10**): Der Server *liest* für
die Anzeige, *entscheidet* aber weiterhin nichts (kein Merge, keine
Ref-Änderung außer über die bestehende API).

- `object(oid)`: Bytes von der Platte, Typ aus `objects`, Request-Cache.
- `resolveRef(name|oid)`: Branch → OID, oder 32-Hex-Commit-OID.
- `treeAt(commit, path)`: Pfad **nur über Tree-Einträge** auflösen (nie über
  das Dateisystem) – kein Pfadausbruch möglich; Namen werden wie Bytes
  behandelt und bei der Ausgabe maskiert.
- `history(startRefs, offset, limit=50)`: Seiten der vorhandenen
  `GraphLayout`-Ausgabe (Walk bis Seitenende statt ganzer Historie).
- `changedFiles(commit)`: Tree-Vergleich mit dem ersten Elternteil
  (hinzugefügt/gelöscht/geändert, rekursiv über OID-Gleichheit; Merge-Commits
  gegen den ersten Elternteil wie `git show --first-parent`).
- `diff(oldBlob, newBlob)`: zeilenweiser Myers-Diff mit 3 Kontextzeilen.
  Grenzen: Dateien > 512 KB oder > 5000 Zeilen → „Diff zu groß“, Binärdateien
  (NUL-Byte in den ersten 8 KB) → „Binärdatei geändert“.
- **Zeichensatz:** Amiga-Textdateien sind meist **ISO-8859-1**. Ist ein Blob
  kein gültiges UTF-8, wird er für die Anzeige als Latin-1 nach UTF-8
  umgewandelt (Hinweis „Latin-1“ an der Datei); Download liefert immer die
  Originalbytes.
- `zip(commit, path)`: Tree rekursiv in ein ZIP (Temp-Datei, 1-GB-Grenze, wie
  AmiCloud), Name `<repo>-<branch|kurz-oid>.zip`.

### 4.3 Zugriff und Adressierung

Repos werden im Web als **`<besitzer>/<repo>`** adressiert (öffentliche Repos
verschiedener Benutzer dürfen gleich heißen). Eine Funktion
`RepoAccess::forView(owner, name)` liefert das Repo, wenn es öffentlich ist
**oder** dem angemeldeten Benutzer gehört – sonst überall **404** (privat und
nicht vorhanden sind nicht unterscheidbar). Eigentümer-Aktionen
(Einstellungen, Löschen) prüfen zusätzlich Eigentum + CSRF.

### 4.4 API (abwärtskompatibel)

- Basic Auth akzeptiert zusätzlich App-Passwörter; Login-Bremse → 429.
- `update-ref` setzt `repos.pushed_at`.
- `create-repo` unverändert (neue Repos privat). Keine weiteren Actions; die
  API bleibt Eigentümer-only, auch für öffentliche Repos.

### 4.5 Seiten (`web.php` als Controller, Ansichten in `includes/web/`)

| Seite | Inhalt |
|---|---|
| **Startseite** `index.php` | Logo (Boing-Ball mit Branch-Linien), Kurzbeschreibung, „Anmelden“, Liste **öffentlicher Repositories**, Einrichtungshinweis für `amivcs.cfg` |
| **Entdecken** | alle öffentlichen Repos (auch ohne Anmeldung), Suche nach Name/Beschreibung |
| **Meine Repositories** | Liste mit Beschreibung, Öffentlich/Privat-Badge, Branches, letzter Push; „Neues Repository“ (Name, Beschreibung, Sichtbarkeit) |
| **Repo → Code** | Kopf (besitzer/repo, Badge, Beschreibung, Klon-Hinweis `amivcs clone <repo>` für den Eigentümer), Branch-/Commit-Auswahl, Brotkrumen, Dateibaum (Name, Größe), README (`README`, `README.txt`, `README.md`, `ReadMe`, als Text) unter dem Baum, „ZIP herunterladen“ |
| **Repo → Datei** | Zeilennummern, Latin-1-Hinweis, „Roh“/„Herunterladen“; PNG/JPEG/GIF als Bild, andere Binärdateien nur Download |
| **Repo → Historie** | Commit-Graph (vorhandene Lane-Darstellung), alle Branches oder einer, 50 je Seite |
| **Repo → Commit** | Nachricht, Autor, Datum, Eltern (Links), Liste der geänderten Dateien mit +/−, Diffs (einklappbar, große zu) |
| **Repo → Branches** | alle Branches mit letztem Commit, Datum, Standard-Markierung |
| **Repo → Einstellungen** (Eigentümer) | Beschreibung, Sichtbarkeit, Standard-Branch, Repository löschen (Namen eintippen) |
| **Persönliche Einstellungen** | Profil, Passwort, App-Passwörter (fertige `amivcs.cfg`-Zeilen) |
| **Benutzer** (Admin) | wie AmiCloud, ohne Kontingent; zeigt Anzahl Repos |

Rohdaten/Downloads gehen immer mit `Content-Disposition: attachment`,
`nosniff` und `CSP: sandbox` raus – nur PNG/JPEG/GIF werden inline mit
passendem Typ ausgeliefert (kein SVG/HTML inline).

## 5. Clients

Keine Protokolländerung. Anpassungen:
- `AMIVCSPrefs`: Feldbeschriftung „(App-)Passwort“ (wie AmiCloudPrefs).
- Fehlermeldungen 429/401 kommen über den vorhandenen `message`-Pfad an.
- Core bleibt unverändert.

## 6. Dokumentation

ADR 10 (lesende Server-Anzeige, öffentliche Repos), `docs/INSTALL.md`
(Admin-CLI statt SQL, `migrate_001`, Startseite, `.user.ini`), Handbuch
(Weboberfläche, App-Passwörter), `README.md`, `config-examples`.

## 7. Deploy

1. Backup `/var/www/amivcs` + `migrate_001.sql` als MariaDB-root.
2. Kompletten `server/`-Stand hochladen (ohne `config.php`), Rechte setzen;
   `AMIVCS_ALLOW_HTTP` in der Produktions-Config prüfen/entfernen.
3. `ronny` zum Admin machen (falls das Konto existiert, sonst anlegen lassen).
4. Prüfen: API mit echtem `amivcs`-Client (push/pull), Web-Rundgang lesend.

## 8. Tests

- Lokal: MariaDB + `php -S`; Test-Repo mit dem echten `amivcs`-PC-Client
  erzeugen und pushen: mehrere Branches, Merge-Commit, Unterordner,
  Latin-1-Datei, Binärdatei, Bild, Umbenennung, große Datei.
- Browser-Tests per CDP (wie AmiCloud): Navigation Baum/Datei/Historie/
  Commit/Diff, Sichtbarkeit (anonym sieht öffentlich, nicht privat; anderer
  Benutzer bekommt 404), Einstellungen, Löschen, App-Passwort mit `amivcs push`.
- `php -l`, manuelle Prüfung auf PHP-8-Syntax (Server läuft 8.3, Code bleibt
  7.4-tauglich).

## 9. Risiken

- **Leistung:** Historie und Diffs lesen Objekte einzeln von der Platte. Für
  die Repo-Größen auf dem Amiga unkritisch; Seiten à 50 Commits, Diff-Grenzen.
- **Leck privater Repos:** zentrale Zugriffsprüfung (§4.3), Tests mit
  anonymem und fremdem Benutzer.
- **Bösartige Objekte:** Decoder prüft Längen (vorhanden); Baumtiefe begrenzt
  (max. 64), Zyklen sind wegen inhaltsadressierter OIDs nicht konstruierbar,
  werden aber trotzdem per Besucht-Liste abgefangen.
