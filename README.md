# AmiVCS

*AmiVCS is a small, Amiga-optimised version control system for AmigaOS 3.x
and PC (macOS/Linux/Windows): a command line client, git-gui-style
graphical front-ends (MUI on the Amiga, Qt 6 on the PC) and a lean
PHP server that only stores objects and branch refs. It is deliberately
not a Git clone, but borrows Git's proven ideas (commit/tree/blob,
branches, staging, 3-way merge).*

> **Status:** experimental. Developed with the help of Claude (an AI assistant
> by Anthropic), primarily for the author's own use. Try it with copies of
> your data first; use at your own risk.

AmiVCS ist eine kleine, bewusst einfach gehaltene Versionsverwaltung,
die auf einem Amiga mit 68020-CPU genauso läuft wie auf einem aktuellen
PC. Fast alle Befehle (`init`, `status`, `add`, `commit`, `log`,
`branch`, `switch`, `merge`, `gc` …) arbeiten rein lokal im
Arbeitsverzeichnis – ein Server wird nur für `clone`/`push`/`pull`
gebraucht. Der mitgelieferte Server ist „dumm“: Er speichert nur Objekte
und Branch-Zeiger; jede Merge-Entscheidung fällt auf dem Client.

## Komponenten

| Programm / Teil | Plattform | Zweck |
|---|---|---|
| `amivcs` (`apps/amivcs`) | Amiga + PC | Kommandozeilen-Client: init, status, add, remove, commit, log, branch, switch, checkout, merge, gc, cat-file, clone, push, pull |
| `amivcs-gui` (`apps/amivcs-gui-mui`) | Amiga (MUI 3.8+) | git-gui-artige Oberfläche: Vormerken, Commit, Branches, Historie mit Commit-Graph, Clone/Push/Pull |
| `amivcs-gui` (`apps/amivcs-gui-qt`) | PC (Qt 6) | dieselbe Oberfläche für macOS/Linux/Windows |
| `AMIVCSPrefs` (`apps/amivcs-prefs`) | Amiga | Einstellungs-Editor für `ENV:`/`ENVARC:amivcs.cfg` |
| `libs/libcloudvcs` | beide | Objektmodell, Objektspeicher, Index, Merge, Packs, Remote-Protokoll |
| `libs/libcloudvcsgui` | beide | GUI-neutrale Steuerschicht (Sitzung, Graph-Layout) für beide Oberflächen |
| `server/` | Webserver (PHP + MariaDB) | AmiVCS-Server: `api.php` (Objekt-/Ref-Speicher für die Clients), `web.php` (Weboberfläche im Stil von Gitea: Code, Historie, Diffs, öffentliche/private Repos, App-Passwörter, Benutzerverwaltung) |
| `core/` | – | [AmiCloudCore](https://github.com/dangelrk/AmiCloudCore) als Git-Submodul (libcloudcore, libcloudhttp, libcloudfs, gemeinsame Build-Logik) |

## Schnellstart

```sh
git clone --recurse-submodules https://github.com/dangelrk/AmiVCS.git
cd AmiVCS
make pc                       # amivcs für den PC  -> bin/pc/
make qt                       # amivcs-gui (Qt 6)  -> bin/pc/
export VBCC=/pfad/zu/vbcc
make amiga                    # Amiga-Programme    -> bin/amiga/
make dist-amiga               # Installationsschublade build/dist/AmiVCS/
make release                  # build/release/AmiVCS-<version>.lha + .readme
```

Auf dem Amiga in der Schublade das Icon `Install-AmiVCS` doppelt anklicken
(Commodore-Installer; ohne Installer `Execute Install-AmiVCS.shell`).

Wurde ohne `--recurse-submodules` geklont: `git submodule update --init`.

Erste Schritte mit dem Programm:

```sh
cd meinprojekt
amivcs init
amivcs commit -a -m "Erster Stand"
amivcs log
```

## Dokumentation

- [docs/BUILD.md](docs/BUILD.md) – Bauen: Amiga (Cross-Compile mit vbcc
  von macOS/Linux/Windows, nativ mit `Build-Amiga`) und PC (CLI + Qt)
- [docs/INSTALL.md](docs/INSTALL.md) – Installation auf Amiga und PC sowie
  Einrichtung des AmiVCS-Servers
- [docs/user/AmiVCS-Handbuch.html](docs/user/AmiVCS-Handbuch.html) –
  Benutzerhandbuch (HTML)
- [docs/user/AmiVCS.guide](docs/user/AmiVCS.guide) – dasselbe als
  AmigaGuide
- [config-examples/README.md](config-examples/README.md) –
  Konfigurationsdateien
- [docs/adr/09_ADR_VCS_Design.md](docs/adr/09_ADR_VCS_Design.md) –
  Architekturentscheidung; [docs/design/](docs/design/) – historische
  Planungsdokumente

## Schwesterprojekte

AmiVCS ist aus einem gemeinsamen Monorepo hervorgegangen und teilt sich
die C-Bibliotheken mit:

- [AmiCloudCore](https://github.com/dangelrk/AmiCloudCore) – gemeinsame
  Bibliotheken und Build-Logik (hier als `core/` eingebunden)
- [AmiCloud](https://github.com/dangelrk/AmiCloud) – eigener Cloud-Server,
  CLI, Dateimanager und `AMICLOUD:`-Handler
- [AmiOwnCloud](https://github.com/dangelrk/AmiOwnCloud) – OwnCloud/
  Nextcloud per WebDAV: `OWNCLOUD:`-Handler und AmiOwnCloudSync

AmiVCS ist von AmiCloud vollständig getrennt: eigener Server, eigene
Datenbank, eigene Konfigurationsdatei, eigene Zugangsdaten.

## Lizenz

Copyright (C) 2026 Ronny Krueger

AmiVCS ist freie Software unter der **GNU General Public License,
Version 3 oder später** (GPL-3.0-or-later) – Volltext in [LICENSE](LICENSE).
Die im Submodul AmiCloudCore enthaltene JSON-Bibliothek Parson steht
unter der MIT-Lizenz.
