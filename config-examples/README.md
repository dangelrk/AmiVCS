# Konfigurationsbeispiele

AmiVCS kennt eine globale Client-Konfiguration, eine pro Repository und
die Server-Konfiguration. Alle sind von AmiCloud und AmiOwnCloud
vollständig getrennt – keine gemeinsamen Server, keine gemeinsamen
Zugangsdaten.

| Datei | Ablage im Betrieb | Gelesen von | Geschrieben von |
|---|---|---|---|
| [config.example](config.example) (global) | Amiga: `ENV:AmiVCS/config` (aktiv) und `ENVARC:AmiVCS/config` (dauerhaft); macOS/Linux: `~/.amivcsconfig`; Windows: `%USERPROFILE%\.amivcsconfig` | `amivcs` (CLI), `amivcs-gui` (MUI und Qt), `AmiVCSConfig` | `AmiVCSConfig` (`apps/amivcs-config`, Amiga), `amivcs config --global`, Qt-Einstellungsdialog oder von Hand |
| `.amivcs/config` (pro Repository) | im Repository, nicht versioniert | dieselben | `amivcs clone`/`push <url>`/`remote`/`config`, die Login-Dialoge der GUIs, `AmiVCSConfig` |
| [`server/config.php.example`](../server/config.php.example) | `config.php` im Web-Root des Servers | `server/api.php`, `server/web.php` | von Hand (siehe [docs/INSTALL.md](../docs/INSTALL.md#3-amivcs-server-einrichten)) |

Format beider Client-Dateien: eine Einstellung pro Zeile als
`schlüssel=wert` im lokalen Zeichensatz, Zeilen mit `#` am Anfang sind
Kommentare.

## Globale Vorgaben (ENVARC:AmiVCS/config, ~/.amivcsconfig)

Rein lokal (init/add/commit/log/branch/merge) wird nur der Name
gebraucht – ohne ihn lehnt `amivcs commit` ab, wie git.

| Schlüssel | Bedeutung |
|---|---|
| `name` | Autor der Commits (wie `user.name` bei git); ein Repository kann einen eigenen haben |
| `email` | E-Mail des Autors (wie `user.email`); der Autor erscheint als `Name <email>` |
| `verify_ssl` | `1` (Standard) prüft das TLS-Zertifikat, `0` nur zum Testen |
| `timeout` | Netz-Timeout in Sekunden (Standard `120`): so lange darf eine Verbindung ganz ohne Datenfluss bleiben; nie eine Grenze für einen ganzen `clone`/`push`/`pull` |
| `charset` | lokaler Zeichensatz der Dateinamen (Amiga, mit codesets.library) |
| `debug` | Diagnoseausgaben |

`AmiVCSConfig` schreibt mit **Speichern** `ENV:` und `ENVARC:`, mit
**Abbrechen** nichts (die Werte des Repositorys sind ohnehin eine Datei,
ein „Benutzen“ gibt es deshalb nicht mehr).

## Pro Repository: .amivcs/config

| Schlüssel | Bedeutung |
|---|---|
| `remote` | Remote-URL wie bei git: `https://amivcs.example.com/<repository>` – alles vor dem letzten `/` ist der Server (dort liegt `api.php`), dahinter der Name des Repositorys auf dem Server. Auf dem Amiga nur `https://` |
| `user` | Benutzername an diesem Server |
| `pass` | Passwort, **nur** wenn beim Login „Passwort merken“ gewählt wurde – im Klartext; am besten ein App-Passwort aus der Weboberfläche, das sich je Gerät widerrufen lässt. `amivcs config --unset pass` vergisst es |
| `name`, `email` | Autor nur für dieses Repository (leer = globale Vorgabe) |

Fehlen Benutzer oder Passwort oder meldet der Server 401, fragen
`amivcs` (im Terminal, Passwort ohne Echo), `amivcs-gui` und die IDE
(per Dialog) danach. Beim `clone` gibt es noch kein Repository: gefragt
wird vorher, gespeichert im neuen Repository.

Die frühere `amivcs.cfg` (`server=`, `user=`, `pass=`) und
`remote_repo=` in `.amivcs/config` werden seit der Version nach 0.9 nicht mehr
gelesen.

## Server: config.php

Vorlage ist `server/config.php.example`; auf dem Server als `config.php`
im selben Verzeichnis ablegen. Die Konstanten (`AMIVCS_DB_*`,
`AMIVCS_STORAGE_PATH`, `AMIVCS_MAX_UPLOAD`, `AMIVCS_ALLOW_HTTP`) sind in
[docs/INSTALL.md](../docs/INSTALL.md#3-amivcs-server-einrichten)
beschrieben. `config.php` enthält Zugangsdaten und ist per `.gitignore`
von der Versionsverwaltung ausgenommen.

## .amivcsignore

Keine Konfigurationsdatei im engeren Sinn, aber gut zu kennen:
`.amivcsignore` im Repository-Wurzelverzeichnis enthält Muster für
Dateien, die nicht versioniert werden (legt `amivcs init` mit
Standardinhalt an; Syntax im Handbuch).
