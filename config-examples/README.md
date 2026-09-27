# Konfigurationsbeispiele

AmiVCS kennt genau eine Client-Konfigurationsdatei und eine
Server-Konfiguration. Beide sind von AmiCloud und AmiOwnCloud vollständig
getrennt – keine gemeinsamen Server, keine gemeinsamen Zugangsdaten.

| Datei | Ablage im Betrieb | Gelesen von | Geschrieben von |
|---|---|---|---|
| [amivcs.cfg.example](amivcs.cfg.example) | Amiga: `ENV:amivcs.cfg` (aktiv) und `ENVARC:amivcs.cfg` (dauerhaft); PC: `~/.amivcsrc` | `amivcs` (CLI), `amivcs-gui` (MUI und Qt), `AMIVCSPrefs` | `AMIVCSPrefs` (`apps/amivcs-prefs`, Amiga) oder von Hand |
| [`server/config.php.example`](../server/config.php.example) | `config.php` im Web-Root des Servers | `server/api.php`, `server/web.php` | von Hand (siehe [docs/INSTALL.md](../docs/INSTALL.md#3-amivcs-server-einrichten)) |

## amivcs.cfg

Nur für `clone`, `push` und `pull` nötig – bis auf `user`, das auch
lokal als Commit-Autor dient. Suchreihenfolge:

| Programm | Suchreihenfolge |
|---|---|
| `amivcs` auf dem Amiga | `-c <datei>`, sonst `ENV:amivcs.cfg`, sonst `amivcs.cfg` im aktuellen Verzeichnis |
| `amivcs` unter macOS/Linux | `-c <datei>`, sonst `~/.amivcsrc` |
| `amivcs` als Windows-Build | `-c <datei>`, sonst `amivcs.cfg` im aktuellen Verzeichnis |
| `amivcs-gui` (MUI) | `ENV:amivcs.cfg`, sonst `amivcs.cfg` im aktuellen Verzeichnis |
| `amivcs-gui` (Qt) | `~/.amivcsrc` (Windows: `%USERPROFILE%\.amivcsrc`) |
| `AMIVCSPrefs` | `ENV:amivcs.cfg`, sonst `ENVARC:amivcs.cfg` |

Format: eine Einstellung pro Zeile als `schlüssel=wert`, Zeilen mit `#`
am Anfang sind Kommentare. Der Parser stammt aus
`core/libcloudcore/src/config.c` (AmiCloudCore).

| Schlüssel | Bedeutung |
|---|---|
| `server` | Basis-URL des AmiVCS-Servers, z. B. `https://amivcs.example.com` (ohne `/api.php`; ein abschließender `/` wird entfernt). Auf dem Amiga nur `https://` |
| `user` (auch `username`) | Benutzername am Server **und** Commit-Autor in `amivcs log`; `amivcs --author <name>` überschreibt ihn für einen Aufruf |
| `pass` (auch `password`) | Passwort am Server |
| `verify_ssl` | `1` (Standard) prüft das TLS-Zertifikat, `0` nur zum Testen |

Der gemeinsame Parser versteht außerdem `basepath`, `debug`, `gmt_offset`
und `dst`; AmiVCS wertet diese Schlüssel nicht aus.

`AMIVCSPrefs` folgt der klassischen Workbench-Prefs-Semantik:
**Benutzen** schreibt nur `ENV:amivcs.cfg`, **Speichern** schreibt
`ENVARC:amivcs.cfg` und `ENV:amivcs.cfg`, **Abbrechen** schreibt
nichts. Unterscheiden sich `ENV:` und `ENVARC:`, weist die Statuszeile
darauf hin (angezeigt wird dann der Inhalt von `ENV:`).

Beide Oberflächen (`amivcs-gui`) fragen Server, Benutzer und Passwort
beim ersten Clone/Push/Pull ab, wenn sie in der Datei fehlen. Diese
Eingaben gelten nur bis zum Programmende und werden nicht gespeichert.

## Server: config.php

Vorlage ist `server/config.php.example`; auf dem Server als `config.php`
im selben Verzeichnis ablegen. Die Konstanten (`AMIVCS_DB_*`,
`AMIVCS_STORAGE_PATH`, `AMIVCS_MAX_UPLOAD`, `AMIVCS_ALLOW_HTTP`) sind in
[docs/INSTALL.md](../docs/INSTALL.md#3-amivcs-server-einrichten)
beschrieben. `config.php` enthält Zugangsdaten und ist per `.gitignore`
von der Versionsverwaltung ausgenommen.

## Pro Repository: .amivcsignore und .amivcs/config

Keine Konfigurationsdateien im engeren Sinn, aber gut zu kennen:

- `.amivcsignore` im Repository-Wurzelverzeichnis – Muster für Dateien,
  die nicht versioniert werden (legt `amivcs init` mit Standardinhalt
  an; Syntax im Handbuch).
- `.amivcs/config` – merkt sich nach `clone` bzw. `push <repo>` den
  Namen des Server-Repositories (`remote_repo=<name>`), damit spätere
  `push`/`pull` ohne Namen auskommen. Wird von `amivcs` selbst
  geschrieben.
