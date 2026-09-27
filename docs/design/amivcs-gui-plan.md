# AMIVCS-GUI — Umsetzungsplan v2 (Staging, volle Historie, WebUI)

## Context

Erweiterung des zuvor entworfenen `amivcs-gui`-Plans (Qt+MUI, git-gui-Stil).
Drei substanzielle Ergänzungen, vom Nutzer nach Durchsicht des ersten
Entwurfs angefordert:

1. **Vollständige Historie über mehrere Branches** — bisher nur
   Erster-Elternteil-Kette. Konsole bleibt schlicht (Amiga-Fenster ~70–80
   Zeichen breit), aber die **GUI bekommt einen echten visuellen Graphen**
   mit Spalten/Linien wie `gitk`/`git log --graph` (Nutzerentscheidung,
   kein flaches Ersatz-Listing).
2. **Staging-Bereich, dateiweise** — kippt die ursprüngliche ADR-09-
   Entscheidung „kein Staging/Index“ bewusst. `add`/`remove` von Dateien,
   granularität = ganze Datei, kein Zeilen-/Hunk-Staging.
3. **WebUI** — nach Login alle Repos des Nutzers mit Historie auflisten,
   analog zu `server/web.php` bei AmiCloud.

Diese Version **ersetzt** den vorherigen Plan-Entwurf vollständig (nicht nur
Ergänzung) — wo unten nichts anderes steht, gilt weiterhin, was zuvor schon
entworfen war (Bibliotheksaufteilung `libs/libcloudvcsgui/`, Qt+MUI als
Dual-Frontend, `amivcs-gui` als Binary-Name, Fortschritt+echtes Abbrechen
über `ui_hooks.h`). Nur Planung — noch nicht implementiert.

---

## 1. Staging-Bereich (neu, dateiweise)

### Design-Idee: Index = einfach eine Tree-OID

Kein neues Dateiformat nötig — der Staging-Index ist schlicht die OID eines
ganz gewöhnlichen `VCS_OBJ_TREE`-Objekts (existiert schon), gespeichert in
einer einzeiligen Datei `.amivcs/index` (32 Hex-Zeichen, wie `HEAD`). Ein
frisches Repo (oder eins, in dem noch nie gestaged wurde) hat einen Index,
der exakt HEADs Tree spiegelt.

**Neues Modul `libs/libcloudvcs/include/cloudvcs/index.h` + `src/index.c`:**

```c
#define VCS_INDEX_FILE "index"

int vcs_index_load(const char *amivcs_dir, VcsOid *out_tree_oid);
int vcs_index_save(const char *amivcs_dir, const VcsOid *tree_oid);

/* Nach erfolgreichem checkout/switch/merge/pull aufgerufen -- Index
 * spiegelt danach wieder exakt den neuen HEAD (wie bei git). */
int vcs_index_reset_to(const char *amivcs_dir, const VcsOid *tree_oid);

/* Hasht/schreibt relpaths aktuellen Arbeits-Inhalt als Blob und schreibt
 * den Index-Tree (sowie jeden Eltern-Tree entlang des Pfads) neu, sodass
 * er auf die neue OID zeigt. Rekursiv fuer Verzeichnisse. */
int vcs_index_add(VcsRepo *repo, const char *relpath);

/* Entfernt relpath aus dem Index-Tree (wie "git rm --cached") --
 * Arbeitsverzeichnis bleibt unangetastet. */
int vcs_index_remove(VcsRepo *repo, const char *relpath);

/* Bequemlichkeit: staged alle aktuell von vcs_repo_status_raw() als
 * New/Modified/Deleted gemeldeten Pfade in einem Aufruf ("git add -A"). */
int vcs_index_add_all(VcsRepo *repo);
```

`add`/`remove` brauchen intern einen „Tree-Pfad neu aufbauen"-Algorithmus:
Tree-Objekte entlang des Pfads von der Wurzel bis zum Elternverzeichnis von
`relpath` werden dekodiert (`vcs_tree_decode`), der Blatt-Eintrag ersetzt
oder entfernt (`vcs_tree_upsert`), und jede Ebene von unten nach oben neu
kodiert+geschrieben (`vcs_tree_encode`+`vcsobjstore_write`) — bis zur neuen
Wurzel-OID, die dann in `.amivcs/index` landet. Nutzt **ausschließlich**
bereits vorhandene Bausteine, kein neues Objektformat.

### Auswirkung auf `repo.h`/`repo.c`

**Status wird zweigeteilt** (git-Wortlaut, passend zum bestehenden
englischen CLI-Ton):

```c
typedef struct {
    VcsChangeList staged;    /* Index vs. HEAD -- wird committet */
    VcsChangeList unstaged;  /* Arbeitsverzeichnis vs. Index -- noch nicht vorgemerkt */
} VcsStatus;

int  vcs_repo_status_raw(VcsRepo *repo, VcsStatus *out);
void vcs_status_free(VcsStatus *out);
```

`vcs_repo_status()` (CLI-Druckversion) wird zum Wrapper, druckt zwei
Abschnitte: `"Changes to be committed:"` / `"Changes not staged for commit:"`.

**Commit committet jetzt den Index, nicht mehr das Arbeitsverzeichnis:**

```c
int vcs_repo_commit(VcsRepo *repo, const char *author, const char *message,
                    int stage_all, VcsOid *out_oid);
```

`stage_all` (CLI: `-a`/`--all`-Flag, git-Konvention) staged vor dem Commit
automatisch alles — ohne das Flag muss vorher explizit `add` aufgerufen
worden sein. Commit selbst braucht **keinen Arbeitsverzeichnis-Scan mehr**:
Tree-OID kommt direkt aus `.amivcs/index`.

**`vcs_repo_snapshot_workdir()` bleibt erhalten** (wird von `merge.c`
weiterhin gebraucht, siehe unten) — nur der reine `commit()`-Pfad nutzt
sie nicht mehr, sondern den Index.

**Merge/Checkout/Switch/Pull:** nach erfolgreichem Materialisieren eines
neuen Trees wird zusätzlich `vcs_index_reset_to(amivcs_dir, new_tree_oid)`
aufgerufen — Index startet nach jedem Branch-Wechsel/Merge wieder sauber.
Ein konfliktfreier Merge baut seinen Ergebnis-Tree weiterhin über
`vcs_repo_snapshot_workdir()` (Merge betrifft potenziell viele Pfade auf
einmal, nicht Nutzer-gesteuert einzeln — Staging ist dafür kein passendes
Modell), setzt danach aber ebenfalls den Index zurück.

`vcs_repo_is_dirty()` bleibt **unverändert** — vergleicht weiterhin direkt
Arbeitsverzeichnis gegen HEAD, unabhängig vom Index (die Verweigerungslogik
bei switch/checkout braucht nur „gibt es überhaupt eine Differenz", nicht
„wo genau").

### CLI-Ergänzungen (`apps/amivcs/src/main.c`)

| Befehl | Wirkung |
|---|---|
| `amivcs add <pfad>...` | Datei(en) vormerken |
| `amivcs add -A` | Alle Änderungen vormerken |
| `amivcs remove <pfad>...` | Aus der Vormerkung entfernen (Datei bleibt im Arbeitsverzeichnis) |
| `amivcs commit -m <text> [-a]` | Committet Vorgemerktes; `-a` merkt vorher alles automatisch vor |
| `amivcs status` | Zeigt jetzt „Changes to be committed" + „Changes not staged for commit" |

**Bestätigt:** `amivcs commit -m` ohne vorheriges `add` committet ab jetzt
nichts mehr (git-Konvention) — anders als bisher, wo automatisch der
komplette Arbeitsbaum committet wurde. Wer committen will, muss entweder
explizit `add`en (CLI) oder die Dateien in der GUI in den Staging-Bereich
verschieben. `-a` bleibt die Ausnahme-Bequemlichkeit für „alles committen,
ohne einzeln zu adden".

---

## 2. Vollständige Multi-Branch-Historie + echter Graph

### Neue Rohdaten — `repo.h`/`repo.c`

```c
typedef struct {
    VcsOid oid;
    VcsOid parents[VCS_MAX_PARENTS];
    int    nparents;
    long   mtime;
    char   author[64];
    char  *message;
    char   decoration[256]; /* kommagetrennte Branch-Namen, die genau hier zeigen, "" wenn keine */
} VcsLogEntry;
typedef struct { VcsLogEntry *items; int n, cap; } VcsLogList;

/* Nur erster Elternteil ab HEAD -- bleibt CLI's `log`, schmal/konsolentauglich. */
int vcs_repo_log_raw(VcsRepo *repo, VcsLogList *out);

/* JEDER von IRGENDEINEM Branch aus erreichbare Commit, je einmal (OID-Set-
 * Deduplizierung wie beim Objekt-Walk in remote.c), nach Datum absteigend
 * sortiert -- wie "git log --all --date-order". Fuer die GUI-Historie. */
int vcs_repo_log_all_raw(VcsRepo *repo, VcsLogList *out);

void vcs_loglist_free(VcsLogList *list);
```

(Ersetzt die im ersten Entwurf skizzierte einfachere `VcsLogEntry` mit nur
einem `parent`-Feld.)

### Graph-Layout — neu in `libs/libcloudvcsgui/`

Die Spalten-/Linien-Zuordnung (welcher Commit in welcher „Lane" steht, wohin
die Verbindungslinien zu den Elternteilen laufen) ist **reine
Darstellungslogik ohne VCS-Entscheidungscharakter** — gehört daher in die
gemeinsame GUI-Bibliothek, nicht nach `libcloudvcs`, wird aber von **beiden**
Frontends (Qt zeichnet, MUI baut daraus ASCII-Art) genutzt:

```c
typedef struct {
    int lane;                            /* Spalte dieses Commits */
    int parent_lanes[VCS_MAX_PARENTS];   /* Ziel-Spalte(n) der Linien nach unten */
    int nparent_lanes;
} VcsGraphRow;
typedef struct { VcsGraphRow *items; int n; int max_lanes; } VcsGraphLayout;

int  vcsgui_compute_graph_layout(const VcsLogList *log, VcsGraphLayout *out);
void vcsgui_graph_layout_free(VcsGraphLayout *out);
```

Algorithmus (Standard-Lane-Packing, wie in leichtgewichtigen
Git-Graph-Implementierungen üblich): Commits in Datums-Reihenfolge
durchlaufen, eine Liste „aktiver Lanes" führen (jede wartet auf eine
bestimmte Eltern-OID); trifft ein Commit auf eine wartende Lane, übernimmt
er deren Spalte und die Lane wartet fortan auf seinen ersten Elternteil; bei
einem zweiten Elternteil (Merge-Commit) wird eine zusätzliche Lane
eingefügt/wiederverwendet. Freiwerdende Lanes (Root-Commit erreicht) werden
für spätere Branches wiederverwendet, damit der Graph nicht unbegrenzt breit
wird.

### Darstellung je Frontend

- **Qt:** eigener `QStyledItemDelegate` für die Graph-Spalte, zeichnet pro
  Zeile Linien/Punkte per `QPainter` aus `VcsGraphLayout` — daneben normale
  Textspalten (Datum/Autor/Nachricht/Branch-Decoration).
- **MUI:** **kein** Custom-BOOPSI/graphics.library-Zeichnen nötig — die
  gleiche `VcsGraphLayout`-Daten werden zu einer kompakten ASCII-Art-Spalte
  (`"| * "`, `"|/  "` — wie `git log --graph`s eigene Textdarstellung)
  zusammengesetzt und einfach als weitere Spalte im bestehenden
  `MUIA_List_DisplayHook` ausgegeben. Passt exakt zur schmalen
  Amiga-Fensterbreite (~70–80 Zeichen) und zur „nur eingebaute
  MUI-Klassen"-Konvention des Commanders.

---

## 3. Auswirkung auf den bisherigen GUI-Funktionsumfang

Die „Änderungsliste" aus dem ersten Entwurf wird **zweigeteilt** (ersetzt
die dortige Aussage „kein Staged/Unstaged-Split nötig" — das war nur
richtig, solange es keinen Staging-Bereich gab):

- **Nicht vorgemerkte Änderungen** (`unstaged`) — Vormerken-Button/
  Doppelklick verschiebt einzelne Dateien in den Staging-Bereich.
- **Vorgemerkte Änderungen** (`staged`) — Entfernen-Button verschiebt
  zurück; „Alles vormerken"-Knopf für Bequemlichkeit.
- **Commit** committet nur, was in `staged` steht.
- **Historie** zeigt jetzt den vollen Multi-Branch-Graphen (§2), nicht mehr
  nur die Erster-Elternteil-Kette.

`libcloudvcsgui`s Session-Struktur entsprechend erweitert:

```c
typedef struct {
    ...
    VcsChangeList  staged, unstaged;  /* ersetzt das fruehere flache "changes" */
    VcsLogList     log_all;           /* volle Multi-Branch-Historie */
    VcsGraphLayout graph;             /* aus log_all berechnet */
    ...
} VcsGuiSession;

int vcsgui_add(VcsGuiSession *s, const char *relpath);
int vcsgui_remove(VcsGuiSession *s, const char *relpath);
int vcsgui_add_all(VcsGuiSession *s);
int vcsgui_commit(VcsGuiSession *s, const char *message, int stage_all, VcsOid *out_oid);
```

---

## 4. Neu: AMIVCS-WebUI (`server/web.php`)

Analog zu `server/web.php` bei AmiCloud — **eigene** Session/Login, eigene
Tabellen, kein gemeinsamer Code mit dem Cloud-Server.

### Login (exakt das bei `server/web.php` verifizierte Muster)

```php
session_set_cookie_params(['lifetime'=>0,'path'=>'/','secure'=>true,'httponly'=>true,'samesite'=>'Strict']);
session_start();
// CSRF-Token in $_SESSION, per hash_equals() bei POST geprueft
// password_verify() gegen amivcs.users.pass_hash
// session_regenerate_id(true) nach erfolgreichem Login
// session_destroy() bei Logout
```

Getrennt von `api.php`s zustandslosem HTTP-Basic-Auth (für die C-Clients) —
eigene, formularbasierte Session für Menschen im Browser, wie beim
Cloud-Server.

### Seiten (v1-Umfang)

1. **Repo-Liste** (nach Login) — `SELECT id, name, created_at FROM repos
   WHERE owner_id=?`, je Zeile Branch-Anzahl + letzter Commit (Kurzfassung).
2. **Repo-Detail** — Branch-Liste (`refs`-Tabelle) + volle Multi-Branch-
   Historie fürs gewählte Repo, als einfache HTML/CSS-Tabelle mit
   Lane-Indikatoren (kleine farbige Kreise/Balken je Spalte) — **kein**
   interaktiver JS-Graph, sondern serverseitig vorgerechnet, gleiche
   Lane-Idee wie §2, nur in PHP statt C.

**Bewusst NICHT in v1:** keine Datei-/Baum-Browser-Ansicht, kein Diff-View,
kein Commit/Push/Pull aus dem Browser heraus, keine Repo-Anlage über die
WebUI (bleibt API/CLI-Sache) — ein Repo-Überblick mit Historie, kein
GitHub-Nachbau.

### Neue serverseitige Bausteine

- `includes/ObjectDecoder.php` — liest Tree-/Commit-Objekte direkt aus dem
  Storage-Verzeichnis und dekodiert das binäre Format (Varint-Lesen,
  Tree-/Commit-Layout aus `object.c` in PHP nachgebildet) — **rein lesend**,
  nur für die Anzeige.
- `includes/GraphLayout.php` — PHP-Portierung des gleichen
  Lane-Zuordnungs-Algorithmus aus §2.

**Architektur-Hinweis:** ADR 09 legt fest, dass der Server keinerlei
VCS-Logik besitzt (Merge-/Ancestry-Entscheidungen). Objekte für eine reine
Nur-Lese-Anzeige zu dekodieren ist etwas anderes (Darstellung, keine
Entscheidung) — aber es ist neuer Code, den die ursprüngliche ADR nicht
vorgesehen hat. Bewusst als schmale, ausdrücklich gewünschte Ausnahme
markiert, nicht stillschweigend eingeführt.

---

## 5. Geplante Implementierungsreihenfolge (vollständig, ersetzt v1)

1. **Staging-Modul** (`index.h`/`index.c`) + zweigeteilter Status +
   `commit`-Umbau + CLI `add`/`remove`/`-a`. Größte, grundlegendste
   Änderung — CLI-Regressionstest mit Bedacht (Verhaltensänderung bei
   `commit` ohne `add`, siehe §1).
   *Commit: "libcloudvcs: file-level staging area (add/remove/index)".*
2. **Volle Multi-Branch-Historie** (`vcs_repo_log_all_raw` + Decorations).
   *Commit: "libcloudvcs: full multi-branch history with decorations".*
3. **Graph-Layout-Algorithmus** in `libs/libcloudvcsgui/` — reine
   Datenberechnung, host-testbar unabhängig von jeder GUI.
   *Commit: "libcloudvcsgui: commit graph lane layout".*
4. **`libcloudvcsgui`-Session-Kern** + Cancel/Progress-Anbindung (wie im
   ersten Entwurf). *Commit: "libcloudvcsgui: headless GUI session/controller".*
5. **Qt-Frontend** — inkl. Staged/Unstaged-Ansicht + gezeichnetem Graphen.
   *Commit: "amivcs-gui-qt: Qt6 git-gui-style frontend".*
6. **MUI-Frontend** — inkl. Staged/Unstaged-Ansicht + ASCII-Graph-Spalte.
   *Commit: "amivcs-gui-mui: MUI 3.8 frontend".*
7. **AMIVCS-WebUI** (`server/web.php` + `ObjectDecoder.php` +
   `GraphLayout.php`). *Commit: "amivcs-server: web UI (repo list + history)".*
8. **Doku** — GUI- und WebUI-Kapitel in `AMIVCS-Handbuch.html`/`.guide`.
   *Commit: "docs: AMIVCS GUI + WebUI chapters".*

Stufen 1–3 härten die Bibliothek unabhängig von jeder GUI und laufen zuerst.

---

## 6. Verifikation

**Stufe 1 (Staging):** frisches Repo → `status` zeigt nichts; Datei anlegen
→ `status` zeigt sie unter „not staged"; `add` → wandert zu „to be
committed"; `commit -m` ohne `-a` committet nur das Vorgemerkte; zweite
Datei anlegen, NICHT adden, `commit -m` → bleibt unstaged, kein Fehler;
`commit -a` staged+committet alles verbleibende. Nach `switch`/`checkout`
ist der Index sauber (kein Datei fälschlich weiter „staged").

**Stufe 2 (Historie):** Repo mit zwei divergierenden Branches + einem Merge
anlegen; `vcs_repo_log_all_raw()` muss beide Linien plus den Merge-Commit
zeigen, jeder Commit genau einmal, mit korrekten Branch-Decorations.

**Stufe 3 (Graph):** gleiches Test-Repo — Lane-Zuordnung von Hand
nachvollziehen (Merge-Commit hat zwei `parent_lanes`, die richtige Spalte
wird nach Erreichen der Wurzel wiederverwendet).

**Stufen 5–6 (GUI):** wie im ersten Entwurf, ergänzt um: Datei vormerken/
zurücknehmen, teilweisen Commit durchführen (nur ein Teil der Änderungen),
Graph-Ansicht mit einem echten Merge-Szenario visuell prüfen.

**Stufe 7 (WebUI):** Login mit Session-Cookie, CSRF-Schutz bei falschem
Token testen, Repo-Liste zeigt nur eigene Repos, Historie-Ansicht stimmt
mit der GUI-Ansicht desselben Repos überein.

**Regressions-Sicherung:** bestehende CLI-Tests aus der vorherigen Sitzung
(init/branch/switch/checkout/merge/gc/clone/push/pull) müssen — abgesehen
von der bewusst geänderten `commit`-Semantik (§1) — unverändert funktionieren.
