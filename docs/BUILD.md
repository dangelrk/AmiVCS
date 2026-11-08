# AmiVCS bauen

Diese Anleitung beschreibt, wie AmiVCS aus den Quellen gebaut wird:

- [1. Quellen holen](#1-quellen-holen)
- [2. Überblick über die Make-Ziele](#2-überblick-über-die-make-ziele)
- [3. PC: macOS, Linux, Windows](#3-pc-macos-linux-windows)
- [4. Amiga: Cross-Compile mit vbcc](#4-amiga-cross-compile-mit-vbcc)
- [5. Amiga: nativ bauen mit Build-Amiga](#5-amiga-nativ-bauen-mit-build-amiga)
- [6. Installationsschublade und Archiv](#6-installationsschublade-und-archiv)
- [7. Testen im Emulator (FS-UAE)](#7-testen-im-emulator-fs-uae)

Die Installation der fertigen Programme steht in [INSTALL.md](INSTALL.md).

## 1. Quellen holen

AmiVCS bindet die gemeinsamen Bibliotheken aus
[AmiCloudCore](https://github.com/dangelrk/AmiCloudCore) als Git-Submodul
unter `core/` ein. Deshalb mit Submodul klonen:

```sh
git clone --recurse-submodules https://github.com/dangelrk/AmiVCS.git
cd AmiVCS
```

Wurde ohne Submodul geklont, nachträglich:

```sh
git submodule update --init
```

Fehlt `core/`, bricht `make` sofort mit genau diesem Hinweis ab.

## 2. Überblick über die Make-Ziele

Alle Ziele werden im Projektverzeichnis aufgerufen (`make help` zeigt sie):

| Ziel | Wirkung |
|---|---|
| `make pc` | Kommandozeilenprogramm `amivcs` für den Host → `bin/pc/` |
| `make qt` | Qt-6-Oberfläche `amivcs-gui` per CMake (Build-Ordner `build/amivcs-gui-qt/`) → `bin/pc/` |
| `make amiga` | Amiga-Programme `amivcs`, `amivcs-gui`, `AmiVCSConfig` per vbcc-Cross-Compiler → `bin/amiga/` |
| `make dist-amiga` | baut `make amiga` und legt die Installationsschublade `build/dist/AmiVCS/` mit Icons an, dazu `build/AmiVCS.lha` |
| `make release` | wie `dist-amiga`, dazu `build/release/AmiVCS-<version>.lha` und `.readme` |
| `make build-amiga-script` | erzeugt das native Amiga-Buildskript `Build-Amiga` neu |
| `make verify-amiga-script` | erzeugt `Build-Amiga` und baut es testweise auf dem Host mit vbcc nach |
| `make check-guide` | AmigaGuide-Handbuch in `docs/user/` prüfen (Knoten, Verweise, Latin-1, Zeilenlänge) |
| `make fsuae [PROGS="…"]` | Amiga-Programme im Emulator FS-UAE testen (siehe unten) |
| `make install` | kopiert alle Programme aus `bin/pc/` nach `$(PREFIX)/bin` (Standard `/usr/local`, `DESTDIR` wird unterstützt) |
| `make clean` | entfernt alle Build-Ergebnisse (`bin/`, `lib/`, `obj/`, `build/`) |

Jedes Programm lässt sich auch einzeln bauen, z. B.:

```sh
make -C apps/amivcs -f Makefile.pc                 # CLI für den PC
make -C apps/amivcs -f Makefile.amiga-cross        # CLI für den Amiga
make -C apps/amivcs-gui-mui -f Makefile.amiga-cross
make -C apps/amivcs-prefs -f Makefile.amiga-cross
cmake -S apps/amivcs-gui-qt -B build/amivcs-gui-qt && cmake --build build/amivcs-gui-qt
```

Build-Ausgaben: Programme in `bin/pc/` bzw. `bin/amiga/`, statische
PC-Bibliotheken in `lib/pc/`, Objektdateien in `obj/…` je Komponente,
CMake- und Dist-Ausgaben in `build/`. Alle diese Verzeichnisse sind in
`.gitignore` eingetragen.

## 3. PC: macOS, Linux, Windows

### Voraussetzungen

- C99-Compiler (gcc oder clang) und `make`
- libcurl mit Entwicklerdateien, dazu `pkg-config`
- nur für die Qt-Oberfläche: Qt 6 (Modul Widgets) und CMake ≥ 3.16

| System | Pakete |
|---|---|
| macOS (Homebrew) | `brew install curl pkg-config cmake qt` |
| Debian/Ubuntu | `sudo apt install build-essential pkg-config libcurl4-openssl-dev cmake qt6-base-dev` |
| Fedora/RHEL | `sudo dnf install gcc make pkgconf-pkg-config libcurl-devel cmake qt6-qtbase-devel` |
| Windows (MSYS2) | siehe unten – **ungetestet** |

Getestet wurde der PC-Build bisher nur unter macOS (Apple Silicon). Linux
und Windows sind im Code und in den Makefiles vorgesehen, aber noch nicht
tatsächlich gebaut worden.

### Bauen

```sh
make pc        # -> bin/pc/amivcs
make qt        # -> bin/pc/amivcs-gui
```

`make pc` baut dabei zuerst die benötigten Bibliotheken (`core/libcloudcore`,
`core/libcloudhttp`, `core/libcloudfs`, `libs/libcloudvcs`) nach `lib/pc/`.
`make qt` erledigt das für die Qt-Oberfläche selbst (zusätzlich
`libs/libcloudvcsgui`). Findet `pkg-config` libcurl nicht, suchen die
Makefiles unter `/opt/homebrew` und `/usr/local`.

CMake-Optionen lassen sich über `QT_CMAKE_FLAGS` übergeben (Standard
`-DCMAKE_BUILD_TYPE=Release`), z. B. um ein bestimmtes Qt zu finden:

```sh
make qt QT_CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/qt6"
```

### Windows (MSYS2) – ungetestet

Die Kommandozeilenprogramme setzen eine POSIX-Umgebung voraus. Vorschlag:

- `amivcs` in der MSYS2-Umgebung **MSYS** bauen
  (Pakete: `gcc make pkgconf libcurl-devel`), dann `make pc`.
- Die Qt-Oberfläche in der Umgebung **MINGW64** bzw. **UCRT64** bauen
  (Pakete `mingw-w64-x86_64-qt6-base`, `mingw-w64-x86_64-cmake`,
  `mingw-w64-x86_64-curl` bzw. die `ucrt-x86_64`-Varianten), dann `make qt`.

Dieser Weg ist bisher nicht erprobt; Rückmeldungen sind willkommen.

## 4. Amiga: Cross-Compile mit vbcc

Die Amiga-Programme werden mit **vbcc** für AmigaOS 3.x und eine
68020-CPU übersetzt (seit 0.2; Gleitkomma über `-lmieee`, keine FPU nötig). Das
funktioniert von macOS, Linux und Windows aus.

### Toolchain-Layout

Die Umgebungsvariable `VBCC` zeigt auf die Wurzel der vbcc-Installation.
Die Makefiles erwarten darunter:

| Pfad | Inhalt |
|---|---|
| `$VBCC/bin/` | `vc`, `vbccm68k`, `vasmm68k_mot`, `vlink` |
| `$VBCC/config/aos68k` | vc-Konfiguration für AmigaOS/68k |
| `$VBCC/targets/m68k-amigaos/` | vbcc-Target (`include/`, `lib/`) |
| `$VBCC/sdk/NDK_3.2/Include_H` | AmigaOS NDK 3.2 |
| `$VBCC/sdk/AmiSSL-v5/Developer/include` | AmiSSL-5-SDK |
| `$VBCC/sdk/Roadshow/netinclude` | Roadshow-SDK (bsdsocket-Header) |
| `$VBCC/sdk/MUI_3.8/Developer/C/Include` | MUI-3.8-Entwicklerpaket (nur für `amivcs-gui`) |

Bezugsquellen:

- vbcc, vasm, vlink (Quellen und Target `m68k-amigaos`):
  <http://sun.hasenbraten.de/vbcc/>
- NDK 3.2: von Hyperion Entertainment
- AmiSSL-SDK: aus den AmiSSL-Releases,
  <https://github.com/jens-maus/amissl/releases>
- Roadshow-SDK und MUI-Entwicklerpaket: im Aminet

### Bauen

```sh
export VBCC=/pfad/zu/vbcc
make amiga            # -> bin/amiga/amivcs, amivcs-gui, AmiVCSConfig
```

`make amiga` setzt `$VBCC/bin` selbst an den Anfang von `PATH`. Ohne
gesetztes `VBCC` wird `/opt/vbcc` angenommen.

Auf dem Host gibt es kein `oml` (Amiga-Librarian); jedes Programm
übersetzt die benötigten Bibliotheksquellen deshalb direkt in sein
eigenes `obj/amiga/` und linkt die Objektdateien ohne `.lib`-Zwischenschritt.

### Cross-Compile unter Windows (MSYS2) – ungetestet

Der Amiga-Build ist unter Windows nicht getestet; vorgesehen ist dieser
Weg:

1. [MSYS2](https://www.msys2.org/) installieren und in der Umgebung
   „MSYS“ die Werkzeuge holen: `pacman -S make git python`
   (`python` wird nur für `make build-amiga-script`,
   `make verify-amiga-script` und `make check-guide` gebraucht).
2. vbcc, vasm und vlink als Windows-Programme bereitstellen – aus den
   Quellen von http://sun.hasenbraten.de/vbcc/ selbst übersetzen (z. B.
   mit `gcc` aus MSYS2) oder vorkompiliert beziehen – und mit Target und
   SDKs im oben beschriebenen Layout ablegen, am besten in einem Pfad
   ohne Leerzeichen, z. B. `C:\vbcc`.
3. In der MSYS2-Shell den Pfad in MSYS2-Schreibweise angeben und bauen:

   ```sh
   export VBCC=/c/vbcc
   make amiga
   make dist-amiga
   ```

Auch hier gilt die folgende Stolperfalle: Enthält `config/aos68k`
absolute Pfade, müssen sie zum Windows-Pfad passen (sonst
`VC_CONFIG=…`).

### Stolperfalle: absolute Pfade in `config/aos68k`

Die Datei `$VBCC/config/aos68k` enthält bei vielen Installationen
**absolute Pfade** (etwa auf `startup.o` und die Target-Bibliotheken).
Wurde die Toolchain verschoben oder von einem anderen Rechner kopiert,
schlägt erst der Link-Schritt fehl, typischerweise mit
`Cannot open …/startup.o`. Abhilfe: die Pfade in der Datei anpassen oder
eine korrigierte Kopie angeben:

```sh
make amiga VC_CONFIG=/pfad/zu/meiner/aos68k
make dist-amiga VC_CONFIG=/pfad/zu/meiner/aos68k
```

## 5. Amiga: nativ bauen mit Build-Amiga

Auf einem echten oder emulierten Amiga wird kein `make` gebraucht. Das
Skript `Build-Amiga` im Projektverzeichnis ruft `vc` für jede Quelldatei
einzeln auf und linkt anschließend.

Voraussetzungen auf dem Amiga:

- vbcc mit Target `m68k-amigaos`, `vc` im Suchpfad, NDK-Includes über
  die vbcc-Konfiguration erreichbar
- AmiSSL-5-SDK, Roadshow-SDK (netinclude), MUI-3.8-Entwicklerpaket

Die SDK-Pfade stehen oben im Skript als Variablen:

```
Set AMISSL_INC "AmiSSL:Developer/include"
Set NET_INC    "NetInclude:"
Set MUI_INC    "MUI:Developer/C/Include"
```

Entweder diese Zeilen anpassen oder die Assigns `AmiSSL:`, `NetInclude:`
und `MUI:` passend setzen. Dann im Projektverzeichnis (dort, wo `core/`
liegt):

```
Execute Build-Amiga
```

Ergebnis: `bin/amiga/amivcs`, `bin/amiga/amivcs-gui`,
`bin/amiga/AmiVCSConfig`.

`Build-Amiga` ist eine **erzeugte Datei** – nicht von Hand ändern. Sie
entsteht aus den `Makefile.amiga-cross`-Dateien:

```sh
make build-amiga-script           # Build-Amiga neu erzeugen
make verify-amiga-script          # ... und auf dem Host gegenbauen
```

`verify-amiga-script` übersetzt das Skript nach `sh` und baut damit alle
Programme mit dem Host-vbcc vollständig (alle Programme linken). Auf
echter Amiga-Hardware selbst ist `Build-Amiga` bisher **ungetestet**.

## 6. Installationsschublade und Archiv

```sh
make dist-amiga
```

legt `build/dist/AmiVCS/` an:

```
AmiVCS/
  Install-AmiVCS     Installationsskript (Installer, aus install/amiga/)
  Install-AmiVCS.shell  dasselbe als Shell-Skript
  Uninstall-AmiVCS   Deinstallationsskript
  ReadMe             Kurzbeschreibung
  LICENSE            GPLv3
  C/                 amivcs, amivcs-gui, AmiVCSConfig
  Docs/              AmiVCS.guide
  Examples/          config.example
```

Zu allen Teilen der Schublade erzeugt `core/tools/mkicon.py` Workbench-Icons
nach `install/amiga/icons.spec` (Bild, Standardprogramm, Tooltypes, Position).
Gepackt wird mit `core/tools/lhapack.py` zu `build/AmiVCS.lha` – einem eigenen
LHA-Packer (Methode `-lh5-`, Level-2-Köpfe) in reinem Python, weil es unter
macOS keinen LHA-*Packer* gibt (Homebrews `lha` ist Lhasa und packt nur aus).
Die Archive lassen sich mit Lhasa und mit dem `lha` von AmigaOS 3.2 auspacken.

### Release

```sh
make release
```

baut `make dist-amiga` und legt in `build/release/` die Dateien für
Aminet/GitHub ab: `AmiVCS-<version>.lha` und `AmiVCS-<version>.readme` (aus
`install/amiga/aminet.readme`, `@VERSION@` wird ersetzt). Die Version steht nur in
`core/libcloudcore/include/cloudcore/version.h` (`AC_VERSION`), das Datum
ist immer der Tag des Bauens (`make` übergibt `-DAC_BUILD_DATE=TT.MM.JJJJ`,
`make release` baut alle Amiga-Programme neu). Daraus bauen alle Programme
ihre `$VER`-Angabe, und `make dist-amiga` setzt beide in die Installer-Skripte ein (`@VERSION@`,
`@DATE@`, für die Shell-Variante `@VER_MAJOR@`/`@VER_REVN@`). Weil die
Installer nur ersetzen, wenn die Version im Archiv nicht älter ist, **vor
jedem Release `AC_VERSION` erhöhen**. Ist Lhasa
installiert, prüft `lha t` das Archiv gleich mit.

Weitere
Dateien lassen sich über `DIST_EXTRA` im Top-Level-`Makefile` hinzufügen
(Format `quelle:zielschublade`). Wie die Schublade auf dem Amiga benutzt
wird, steht in [INSTALL.md](INSTALL.md).

Das AmigaGuide-Handbuch ist in ISO-8859-1 kodiert. Nach Änderungen
prüft `make check-guide` (Skript `core/tools/check-guide.py`) Knoten,
Verweise, Kodierung und Zeilenlänge.

## 7. Testen im Emulator (FS-UAE)

`make fsuae` (Skript `core/tools/run-fsuae.sh`) kopiert die gebauten Amiga-Programme aus
`bin/amiga/` in einen Austauschordner und startet FS-UAE. Die Pfade
werden über Umgebungsvariablen eingestellt:

| Variable | Bedeutung | Standard |
|---|---|---|
| `FSUAE_BIN` | FS-UAE-Programm | `fs-uae` aus `PATH`, unter macOS ersatzweise `/Applications/FS-UAE.app/Contents/MacOS/fs-uae` |
| `FSUAE_CONFIG` | FS-UAE-Konfigurationsdatei (`.fs-uae`) | keiner – muss gesetzt werden |
| `FSUAE_SHARED` | Host-Ordner, den die Konfiguration als Laufwerk einbindet | `build/fsuae-shared` |

```sh
export FSUAE_CONFIG=$HOME/amiga/entwicklung.fs-uae
export FSUAE_SHARED=$HOME/amiga/shared
make fsuae                           # alle Programme aus bin/amiga/
make fsuae PROGS="amivcs amivcs-gui"  # nur diese
```

Die FS-UAE-Konfiguration muss `FSUAE_SHARED` selbst als Laufwerk
einbinden (z. B. `hard_drive_1 = …` mit einem Label wie `SHARED`); das
Skript kopiert nur und startet den Emulator.
