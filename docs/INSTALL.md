# AmiVCS installieren

- [1. Amiga](#1-amiga)
  - [Voraussetzungen](#voraussetzungen)
  - [Installation mit dem Install-Skript](#installation-mit-dem-install-skript)
  - [Manuelle Installation](#manuelle-installation)
  - [Konfiguration](#konfiguration-amiga)
  - [Deinstallation](#deinstallation)
- [2. PC (macOS, Linux, Windows)](#2-pc-macos-linux-windows)
- [3. AmiVCS-Server einrichten](#3-amivcs-server-einrichten)

Wie die Programme gebaut werden, steht in [BUILD.md](BUILD.md). Die
Bedienung beschreibt das Handbuch
([HTML](user/AmiVCS-Handbuch.html) bzw. [AmigaGuide](user/AmiVCS.guide)).

Für rein lokale Versionsverwaltung (`init`, `add`, `commit`, `log`,
`branch`, `switch`, `merge` …) wird **kein Server** gebraucht. Server
und Zugangsdaten sind nur für `clone`, `push` und `pull` nötig.

## 1. Amiga

### Voraussetzungen

| Was | Wofür |
|---|---|
| AmigaOS 3.x, 68020 oder besser | alle Programme (übersetzt für 68020 seit 0.2, keine FPU nötig) |
| TCP/IP-Stack mit `bsdsocket.library` (z. B. Roadshow, AmiTCP, Miami) | Clone/Push/Pull in `amivcs` und `amivcs-gui` – lokal wird er nicht gebraucht |
| AmiSSL 5 (`amisslmaster.library`) | wie oben; alle Serververbindungen laufen über HTTPS |
| MUI 3.8 oder neuer (`muimaster.library` ≥ 19) | nur `amivcs-gui` |
| – | `AmiVCSConfig` braucht nur Intuition/GadTools |

Hinweise:

- `amivcs` (Kommandozeile) öffnet Netzwerk und AmiSSL erst bei `clone`,
  `push` oder `pull`. Alle anderen Befehle laufen ohne TCP/IP-Stack.
- `amivcs-gui` ebenso: Netzwerk und AmiSSL werden erst bei Clone, Push
  oder Pull geöffnet. Fehlt dann etwas, zeigt es einen Requester mit
  der Ursache und bleibt geöffnet.
- Stack: Alle drei Programme fordern selbst 64 KB Stack an (vbcc-Variable
  `__stack`, der Startcode legt bei Bedarf einen größeren Stack an).
  Ein `Stack`-Befehl vorher ist nicht nötig.
- Auf dem Amiga sind nur `https://`-Serveradressen möglich.

### Installation mit dem Install-Skript

Das Archiv `AmiVCS-<version>.lha` (bzw. die Schublade aus `make dist-amiga`)
enthält:

```
AmiVCS/
  Install-AmiVCS     Installationsskript (Commodore-Installer, Icon)
  Install-AmiVCS.shell  dasselbe als Shell-Skript
  Uninstall-AmiVCS   Deinstallationsskript (Icon oder Execute)
  ReadMe             Kurzbeschreibung
  LICENSE            GPLv3
  THIRD-PARTY-NOTICES  Lizenz der Fremdkomponente Parson (MIT)
  C/                 amivcs, amivcs-gui, AmiVCSConfig
  Docs/              AmiVCS.guide
  Examples/          config.example
```

Archiv auspacken (z. B. `lha x AmiVCS-0.10.lha RAM:`). Zum Installieren in der Schublade das Icon **Install-AmiVCS** doppelt anklicken.
Es startet den Commodore-Installer (bei AmigaOS 3.2 in `SYS:System`, bei
älteren Systemen von der Install-Diskette nach `C:` kopieren); in einer
Shell geht dasselbe mit `Installer Install-AmiVCS`. Ohne Installer gibt es dasselbe als
Shell-Skript:

```
CD RAM:AmiVCS
Execute Install-AmiVCS.shell
```

Das Skript fragt, welche Teile installiert werden sollen, und

- kopiert `amivcs` und `amivcs-gui` nach `C:`,
- kopiert `AmiVCSConfig` samt Icon nach `SYS:Prefs/` und entfernt das
  alte `SYS:Prefs/AMIVCSPrefs` (bis 0.9),
- kopiert `AmiVCS.guide` nach `HELP:` – nur, wenn der Assign `HELP:`
  existiert,
- prüft, ob `muimaster.library` und `amisslmaster.library` in `LIBS:`
  liegen, und warnt sonst,
- bietet an, `AmiVCSConfig` zu starten, falls noch keine
  `ENVARC:AmiVCS/config` existiert (Name und E-Mail für die Commits),
- weist darauf hin, dass eine alte `ENVARC:amivcs.cfg` nicht mehr
  gelesen wird.

Eine vorhandene Konfiguration wird **nie** überschrieben.

### Aktualisieren

Eine neuere Version installiert man genauso: Archiv auspacken und den
Installer erneut starten. Er vergleicht bei jedem Programm die installierte
Version mit der im Archiv (`$VER`-Angabe, `getversion`):

- **Einsteiger / Geübter Benutzer:** gleiche und ältere Versionen werden
  ersetzt. Ist die installierte Version *neuer*, bleibt sie stehen; der
  Installer nennt sie am Ende.
- **Experte:** für jedes schon installierte Programm zeigt der Installer beide
  Versionen und fragt, ob ersetzt werden soll.

Vorhandene Icons bleiben erhalten (eigene Tooltypes und Positionen gehen
nicht verloren), ebenso Einstellungen und DOSDriver. Die Shell-Variante
`Install-AmiVCS.shell` fragt nur nach, wenn ein installiertes Programm neuer ist
als das im Archiv (Hilfsskript `Install-Datei`, Vergleich mit
`Version … FILE`).

### Manuelle Installation

| Datei aus der Schublade | Ziel | Bemerkung |
|---|---|---|
| `C/amivcs` | `C:` | Kommandozeile |
| `C/amivcs-gui` | `C:` (oder beliebige Schublade) | MUI-Oberfläche |
| `C/AmiVCSConfig` | `SYS:Prefs/` | Einstellungen |
| `Docs/AmiVCS.guide` | `HELP:` oder beliebig | Handbuch |
| `Examples/config.example` | als Vorlage für `ENVARC:AmiVCS/config` | Name/E-Mail, Optionen |

```
Copy C/amivcs C:
Copy C/amivcs-gui C:
Copy C/AmiVCSConfig SYS:Prefs/
Copy Docs/AmiVCS.guide HELP:
```

Es werden keine Icons mitgeliefert. `AmiVCSConfig` lässt sich im Shell
mit `SYS:Prefs/AmiVCSConfig [<repository>]` starten; auf der Workbench ist es nur
sichtbar, wenn im Fenster „Alle Dateien anzeigen“ eingeschaltet ist. Für `amivcs-gui` kann ein
eigenes Projekt-Icon angelegt werden; es versteht die Merkmale
(Tooltypes) `REPO=<pfad>` (zu öffnendes Repository) und `DEBUG`.

Kein DOS-Treiber, kein Mounten: AmiVCS enthält keinen Handler.

<a id="konfiguration-amiga"></a>
### Konfiguration

Es gibt zwei Ebenen, wie bei git:

| Datei | Inhalt |
|---|---|
| `ENV:AmiVCS/config` (aktiv), `ENVARC:AmiVCS/config` (dauerhaft) | globale Vorgaben: `name`, `email` des Autors, `verify_ssl`, `timeout` |
| `.amivcs/config` im Repository | `remote` (Remote-URL, z. B. `https://amivcs.example.com/meinprojekt`), `user`, `pass` (nur mit „Passwort merken“), eigener `name`/`email` |

Rein lokal braucht AmiVCS nur einen Namen für die Commits. Server,
Benutzer und Passwort gehören zu jedem Repository: `amivcs clone <url>`
merkt sich die Adresse, `amivcs push <url>` ebenso (und legt das
Repository auf dem Server an); Benutzer und Passwort werden beim ersten
Netzbefehl gefragt.

**AmiVCSConfig** (`SYS:Prefs/AmiVCSConfig`) bearbeitet beides: die
globalen Vorgaben und das Repository im aktuellen Verzeichnis, im
Argument `<pfad>` oder im Merkmal `REPO=` seines Icons (amivcs-gui und
die AmigaIDE starten es mit dem Repository). **Speichern** schreibt
`ENV:` und `ENVARC:` bzw. `.amivcs/config`, **Abbrechen** nichts.

Dasselbe in der Shell:

```
amivcs config --global name "Vorname Nachname"
amivcs config --global email name@example.org
amivcs remote https://amivcs.example.com/meinprojekt
amivcs config --unset pass
```

Die frühere `amivcs.cfg` und `AMIVCSPrefs` (bis 0.9) werden nicht mehr
benutzt.

Inhalt und alle Schlüssel: [config-examples/README.md](../config-examples/README.md).

### Deinstallation

Aus der Schublade:

```
Execute Uninstall-AmiVCS
```

Das Skript löscht `C:amivcs`, `C:amivcs-gui`, `SYS:Prefs/AmiVCSConfig`
(und ein altes `AMIVCSPrefs`) und `HELP:AmiVCS.guide` und fragt, ob auch
die Einstellungen `ENV:AmiVCS` und `ENVARC:AmiVCS` (und eine alte
`amivcs.cfg`) entfernt werden sollen. Von Hand:

```
Delete C:amivcs C:amivcs-gui SYS:Prefs/AmiVCSConfig HELP:AmiVCS.guide
Delete ENV:AmiVCS ENVARC:AmiVCS ALL
```

Repositories (Verzeichnisse mit einem Unterverzeichnis `.amivcs`)
bleiben dabei selbstverständlich erhalten.

## 2. PC (macOS, Linux, Windows)

Nach `make pc` (und ggf. `make qt`):

```sh
sudo make install                    # nach /usr/local/bin
make install PREFIX=$HOME/.local     # oder ins eigene Verzeichnis
make install DESTDIR=/tmp/paket      # für Paketbau
```

`make install` kopiert alle Programme aus `bin/pc/` (`amivcs`,
`amivcs-gui`). Alternativ die Dateien einfach von Hand an einen Ort im
`PATH` kopieren.

Konfiguration:

globale Vorgaben (Name, E-Mail, Optionen) in `~/.amivcsconfig`, unter
Windows `%USERPROFILE%\.amivcsconfig` – für `amivcs` und `amivcs-gui`
gleichermaßen (Vorlage
[config-examples/config.example](../config-examples/config.example)),
am einfachsten gesetzt mit

```sh
amivcs config --global name "Vorname Nachname"
amivcs config --global email name@example.org
```

Remote-URL, Benutzer und (nur mit „Passwort merken“) Passwort stehen wie
auf dem Amiga in `.amivcs/config` des jeweiligen Repositorys; in
`amivcs-gui` (Qt) unter *Repository > Settings...*. Ein gemerktes
Passwort steht dort im Klartext.

## 3. AmiVCS-Server einrichten

Der Server (`server/`) ist ein reiner Objekt- und Ref-Speicher: Er nimmt
Objekte entgegen, liefert sie aus und aktualisiert Branch-Zeiger per
Compare-and-Swap. Dazu kommt eine Weboberfläche (`web.php`) zum Ansehen
der Repositories und zur Verwaltung von Benutzern und App-Passwörtern. Er hat eine **eigene** Datenbank, ein eigenes
Speicherverzeichnis und eigene Benutzerkonten – unabhängig von einem
AmiCloud-Server auf derselben Maschine.

Voraussetzungen:

- Webserver mit PHP ≥ 7.4 (Erweiterungen `pdo_mysql` und `mbstring`, für
  ZIP-Downloads `zip`), getestet mit Apache
- MariaDB oder MySQL
- eine eigene (Sub-)Domain mit HTTPS-Zertifikat, z. B. per Let's Encrypt.
  Der Server lehnt Klartext-HTTP ab, und `web.php` leitet auf
  `/web.php` um – die Dateien gehören daher in die **Wurzel eines eigenen
  vHosts**, nicht in ein Unterverzeichnis.

Beispiel für AlmaLinux/RHEL mit Apache und dem Platzhalter
`amivcs.example.com` (unter Debian/Ubuntu heißt der Dienst `apache2`, der
Benutzer `www-data`, die vHost-Datei liegt unter `/etc/apache2/sites-available/`):

```sh
# 1. DNS: A-Record amivcs.example.com -> IP des Servers

# 2. vHost /etc/httpd/conf.d/amivcs.conf:
#    <VirtualHost *:80>
#        ServerName   amivcs.example.com
#        DocumentRoot /var/www/amivcs
#        <Directory /var/www/amivcs>
#            Options None
#            AllowOverride All
#            Require all granted
#        </Directory>
#    </VirtualHost>
#    AllowOverride All ist nötig: die .htaccess-Dateien sperren
#    config.php, config.php.example, includes/ und sql/.

# 3. HTTPS-Zertifikat (certbot legt den HTTPS-vHost und die Umleitung an):
sudo certbot --apache -d amivcs.example.com

# 4. Dateien und Speicherverzeichnis (außerhalb des Web-Roots!).
#    "server/." kopiert auch die .htaccess-Dateien mit ("server/*" nicht):
sudo mkdir -p /var/www/amivcs /var/amivcs-storage
sudo cp -r server/. /var/www/amivcs/
sudo cp /var/www/amivcs/config.php.example /var/www/amivcs/config.php
sudo nano /var/www/amivcs/config.php        # DB-Zugang, Speicherpfad
# Code gehört root, Apache darf nur lesen; config.php (Passwort) nur
# für root und die Gruppe apache; schreiben darf Apache nur im Storage:
sudo chown -R root:apache /var/www/amivcs
sudo chmod -R u=rwX,g=rX,o= /var/www/amivcs
sudo chmod 640 /var/www/amivcs/config.php
sudo chown -R apache:apache /var/amivcs-storage
sudo chmod 750 /var/amivcs-storage

# 5. Datenbank "amivcs" samt Tabellen anlegen, DB-Benutzer einrichten:
sudo mysql < /var/www/amivcs/sql/schema.sql
sudo mysql -e "CREATE USER 'amivcs'@'localhost' IDENTIFIED BY 'geheim';
  GRANT SELECT, INSERT, UPDATE, DELETE ON amivcs.* TO 'amivcs'@'localhost';"

# 6. Ersten Administrator anlegen (Passwort wird ohne Echo abgefragt):
cd /var/www/amivcs
sudo -u apache php bin/amivcs-admin.php user:add --admin bob

sudo systemctl reload httpd
```

`config.php` – alle Einstellungen:

| Konstante | Bedeutung |
|---|---|
| `AMIVCS_DB_HOST`, `AMIVCS_DB_NAME`, `AMIVCS_DB_USER`, `AMIVCS_DB_PASS` | Zugang zur Datenbank (Standard-Name `amivcs`, wie in `sql/schema.sql`) |
| `AMIVCS_STORAGE_PATH` | absoluter Pfad des Objektspeichers, **außerhalb** des Web-Roots; Objekte landen unter `<pfad>/<benutzer>/<repo>/objects/<2 hex>/<30 hex>` |
| `AMIVCS_MAX_UPLOAD` | größtes einzelnes Objekt in Bytes (Standard 100 MB) – praktisch die größte versionierbare Datei. `post_max_size` und `upload_max_filesize` in `php.ini` passend setzen |
| `AMIVCS_ALLOW_HTTP` | nur für lokale Tests (`php -S`): erlaubt Klartext-HTTP. **Nie in Produktion** |
| `AMIVCS_TIMEZONE` | optional, Zeitzone der Datumsanzeige in der Weboberfläche (Standard `Europe/Berlin`) |

Unter PHP-FPM setzt die mitgelieferte `.user.ini` die Upload-Grenzen
passend zu 100 MB; unter `mod_php` dieselben Werte in die `php.ini`.

Für Amigas an langsamen Leitungen (Modem; der TLS-Handshake braucht auf
einer 68k-CPU 10–60 Sekunden) sind die Apache-Vorgaben knapp. Empfohlen,
global (die Handshake- und Header-Werte wirken bei namensbasierten vHosts
nur auf Server-Ebene), z. B. in `/etc/httpd/conf.d/01-slow-clients.conf`:

```apache
Timeout 120
RequestReadTimeout handshake=120 header=60-120,MinRate=200 body=60,MinRate=200
KeepAliveTimeout 30
MaxKeepAliveRequests 1000
```

Ein `clone` besteht aus einer Anfrage je Objekt; ohne längeres
Keep-Alive kostet fast jede einen neuen Handshake.

Test:

```sh
curl -u bob:geheim "https://amivcs.example.com/api.php?action=status"
# -> {"status":"ok","version":"0.1"}
```

Danach:

- API für die Clients: `https://amivcs.example.com/api.php` – die
  Clients bekommen aber die Remote-URL eines Repositorys,
  `https://amivcs.example.com/<repository>`, und hängen `api.php` selbst
  an die Basis davor (`amivcs clone https://amivcs.example.com/projekt`)
- Startseite: `https://amivcs.example.com/` (öffentliche Repositories)
- Weboberfläche: `https://amivcs.example.com/web.php` – Dateibaum,
  Dateiansicht, Historie mit Graph, Commits mit Diff, Branches, ZIP-Download;
  Repository-Einstellungen (Beschreibung, öffentlich/privat,
  Standard-Branch, Löschen), persönliche Einstellungen mit App-Passwörtern,
  Benutzerverwaltung für Administratoren.

Repositories entstehen beim ersten `amivcs push https://<server>/<name>` automatisch oder
in der Weboberfläche (Namen: Buchstaben, Ziffern, `_`, `.`, `-`, nicht mit
`.` beginnend, höchstens 128 Zeichen). Neue Repositories sind **privat**;
in den Repository-Einstellungen lassen sie sich öffentlich schalten – dann
kann jeder sie im Browser ansehen und als ZIP laden. `clone`, `push` und
`pull` bleiben immer dem Eigentümer vorbehalten.

Benutzer verwaltet der Administrator in der Weboberfläche (Benutzermenü →
*Benutzer*) oder mit `bin/amivcs-admin.php` (`user:list`, `user:add`,
`user:passwd`, `user:admin <name> on|off`, `user:disable`, `user:enable`,
`user:delete`, `repo:list`), immer als `sudo -u apache php …`. Für die
Clients empfiehlt sich je Gerät ein **App-Passwort** (Weboberfläche →
*Persönliche Einstellungen*). Nach 5 Fehlversuchen pro Benutzer und IP
sperrt der Server 15 Minuten (HTTP 429).

**Nachrüsten einer bestehenden Installation:** `sql/migrate_001.sql` und `sql/migrate_002.sql` als
MariaDB-root einspielen (der Datenbankbenutzer `amivcs` darf kein
`ALTER`), danach einen vorhandenen Benutzer zum Administrator machen:
`sudo -u apache php bin/amivcs-admin.php user:admin bob on`.

Datensicherung: Datenbank (`mysqldump amivcs`) **und**
`AMIVCS_STORAGE_PATH` gemeinsam sichern – die Datenbank enthält nur
Metadaten, die Objekte liegen als Dateien im Speicherverzeichnis.

Lokaler Test ohne Apache (nur zur Entwicklung): in einer lokalen
`config.php` `define('AMIVCS_ALLOW_HTTP', true);` setzen und
`php -S 127.0.0.1:8180 -t server/` starten. Klartext-HTTP können nur
die PC-Clients benutzen; auf dem Amiga ist ausschließlich HTTPS möglich.
Die Web-Oberfläche funktioniert so nicht, weil ihr Sitzungs-Cookie nur
über HTTPS gesendet wird.
