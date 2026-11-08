# AmiVCS: Remote-URL pro Repository, Identität wie bei git, AmiVCSConfig

Stand 07.11.2026 – Plan, vom Nutzer freigegeben.

**Entscheidungen des Nutzers:** keine Rückwärtskompatibilität (AmiVCS ist noch nicht in Benutzung): `server=`/`user=`/`pass=` und `remote_repo=` entfallen ersatzlos. Zugangsdaten **pro Repository** (nicht pro Server), „Passwort merken“ als Option. Identität: zuerst die des Repositorys, sonst die globale Vorgabe (wie git). Global gibt es nur noch Name/E-Mail-Vorgaben (über AmiVCSConfig).

## Ziel

- Jedes Repository kann auf einen anderen AmiVCS-Server zeigen (Remote-URL pro Repository).
- Zugangsdaten werden erst gebraucht, wenn ein Netzbefehl läuft (Clone, Push, Pull) – dann gefragt und auf Wunsch gemerkt, pro Server.
- Name und E-Mail des Autors wie bei git: global, pro Repository überschreibbar.
- AMIVCSPrefs entfällt; an seine Stelle tritt **AmiVCSConfig**, das globale Werte und die Werte des Repositorys bearbeitet, in dem es gestartet wird.
- Server bleibt, wie er ist (er kann schon viele Repositories); nur die Web-UI zeigt die neue Klon-Zeile.

## 1. Remote-URL

Format wie bei git: `https://amivcs.planet-rk.de/<repository>`. Der Client trennt am letzten `/`: davor die Server-Basis (an die er `api.php` hängt), dahinter der Repository-Name. Server in einem Unterverzeichnis gehen damit auch (`https://host/vcs/<repo>`).

Gespeichert in `.amivcs/config`:

```
remote=https://amivcs.planet-rk.de/meinprojekt
```



## 2. Zugangsdaten (pro Repository)

In `.amivcs/config` des Repositorys (nicht versioniert):

```
user=ronny
pass=geheim        # nur mit "Passwort merken"
```

- Gefragt, wenn ein Netzbefehl kein Passwort hat oder der Server 401 meldet: CLI im Terminal (Passwort ohne Echo, danach „merken? j/n“), GUIs/IDE per Dialog mit Häkchen „Passwort merken“.
- Beim Clone gibt es noch kein Repository: gefragt wird vorher, gespeichert wird im neuen Repository.
- Klartext – im Handbuch gesagt.

## 3. Identität (Autor)

Schlüssel `name` und `email`, wie `user.name`/`user.email` bei git:

1. `.amivcs/config` des Repositorys
2. globale Vorgabe (`ENVARC:AmiVCS/config` bzw. `~/.amivcsconfig`)

Commit-Autor: `Name <email>`, ohne E-Mail nur `Name`. Fehlt alles, lehnt `commit` ab und sagt, wie man den Namen setzt (wie git). Die GUIs fragen stattdessen einmal danach und speichern global. `--author` der CLI bleibt.

## 4. CLI

| Befehl | |
|---|---|
| `amivcs clone <url> [verzeichnis]` | Verzeichnis wie bei git aus dem Namen |
| `amivcs push [<url>]` | mit URL: als `remote=` merken (wie bisher mit Name) |
| `amivcs pull [<url>] [branch]` | wie bisher |
| `amivcs remote [<url>]` | zeigt bzw. setzt `remote=` |
| `amivcs config [--global] [<schlüssel> [<wert>]]` | listet/liest/setzt `name`, `email` (global/Repository), `remote`, `user` (Repository); `--unset`; `forget-password` vergisst das gemerkte Passwort |

Für Programme (IDE): `--porcelain-out` meldet fehlende Zugangsdaten als eigenen Datensatz `auth-required <server>`; Zugangsdaten kommen über `--args-from` (nie über die Kommandozeile), mit `--save-credentials`.

## 5. AmiVCSConfig (statt AMIVCSPrefs)

Amiga, GadTools wie bisher (prefsgui), Fensterposition gemerkt:

- **Global**: Name, E-Mail.
- **Dieses Repository** (aktuelles Verzeichnis, Argument `<pfad>` oder Merkmal `REPO=`): Remote-URL, Name/E-Mail nur für dieses Repository (leer = global).
- Benutzername für den Server, „Passwort vergessen“.
- Speichern/Abbrechen (kein „Benutzen“ mehr – die Werte sind Dateien, keine ENV:-Live-Kopie).

Installer: AMIVCSPrefs wird entfernt, AmiVCSConfig nach `SYS:Prefs/`; `amivcs.cfg` wird nicht mehr gelesen.

## 6. amivcs-gui (MUI und Qt)

- Clone fragt die URL (statt Name) und das Zielverzeichnis.
- Push ohne `remote=`: fragt die URL.
- Zugangsdaten-Dialog bei Bedarf (Server, Benutzer, Passwort, „merken“).
- Menü **Repository > Settings...**: MUI startet AmiVCSConfig auf dem Repository; Qt bekommt einen eigenen Dialog (Remote-URL, Name, E-Mail, global/Repository).
- Erster Commit ohne Identität: Dialog Name/E-Mail, global gespeichert.
- Kopfzeile zeigt die Remote-URL.

## 7. AmigaIDE (MUI und Qt)

- Clone fragt die URL; Push ohne Remote fragt die URL (wie heute den Namen).
- `auth-required` aus der CLI → Zugangsdaten-Dialog → Auftrag mit Zugangsdaten wiederholen.
- **Versionen > Einstellungen...** startet AmiVCSConfig auf dem Projekt (MUI); die Qt-IDE bekommt dafür einen kleinen eigenen Dialog, der über `amivcs config` liest und schreibt.

## 8. Server / Web-UI

Klon-Dialog zeigt `amivcs clone https://<server>/<repo>`, ohne Hinweis auf `amivcs.cfg`. Optional: `https://<server>/<repo>` im Browser leitet auf die Repository-Seite um.

## 9. Doku, Tests

- Handbücher (AmigaGuide, HTML), Web-Anleitung, Installer, INSTALL.md.
- `scripts/test_cli.sh` + `scripts/test_server.sh`: URL-Clone, zwei Repositories mit verschiedenen Zugangsdaten, Passwort merken/vergessen, Identität Repository/global.
- Emulator: Clone/Push/Pull über GUI und IDE mit Zugangsdaten-Dialog.

## Reihenfolge

1. libcloudvcs: URL-Zerlegung, Repository- und globale Konfiguration, Zugangsdaten im Repository, Identität.
2. CLI inkl. Porcelain; Tests PC.
3. amivcs-gui MUI + Qt.
4. AmiVCSConfig, Installer.
5. IDE MUI + Qt.
6. Web-UI-Klonzeile, Doku, Emulator-Test, A1200.
