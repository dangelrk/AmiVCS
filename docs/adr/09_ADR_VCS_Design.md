# ADR: VCS-Design

## Ziel

Das Versionsverwaltungssystem (VCS) soll auf der bestehenden Cloud-Architektur aufbauen.

Es soll sich an Git orientieren, jedoch bewusst deutlich einfacher bleiben.

Das Ziel ist ein verständliches, performantes und auf AmigaOS 3 praktikables Versionsverwaltungssystem.

---

# Grundsatz

Das VCS ist **kein Git-Klon**.

Es übernimmt bewährte Konzepte von Git, verzichtet jedoch bewusst auf komplexe Funktionen, die für dieses Projekt keinen wesentlichen Mehrwert bieten.

Die Architektur soll einfach bleiben und sich gut in die bestehende Cloud integrieren.

---

# Unterstützte Funktionen (Version 1)

Version 1 soll ausschließlich folgende Befehle unterstützen:

- init
- status
- commit
- log
- branch
- switch
- checkout
- merge

Diese Funktionen decken die meisten typischen Arbeitsabläufe ab.

---

# Bewusst nicht Bestandteil von Version 1

Folgende Git-Funktionen werden zunächst **nicht** implementiert:

- Staging Area (index)
- rebase
- cherry-pick
- stash
- worktree
- submodule
- bisect
- hooks
- reflog
- signed commits
- sparse checkout
- LFS

Diese Funktionen erhöhen die Komplexität erheblich und sind für die Zielgruppe nicht notwendig.

Sie können später ergänzt werden, ohne die Grundarchitektur zu verändern.

---

# Merge

Merge findet ausschließlich auf dem Client statt.

Der Server besitzt keinerlei Merge-Logik.

In der ersten Version genügt:

- Fast-Forward Merge
- einfacher Drei-Wege-Merge
- Konflikterkennung

Komplexe automatische Merge-Strategien sind ausdrücklich nicht erforderlich.

Konflikte dürfen niemals stillschweigend gelöst werden.

---

# Architektur

Das VCS nutzt die vorhandenen Bibliotheken:

- libcloudcore
- libcloudfs
- libcloudhttp

Später zusätzlich:

- libcloudvcs

Es wird keine Funktionalität doppelt implementiert.

Bereits vorhandene Komponenten (MD5, Dateiscans, HTTP, Revisionen usw.) werden wiederverwendet.

---

# Cloud und VCS

Cloud und VCS sind zwei getrennte Schichten.

Die Cloud stellt den aktuellen Dateistand bereit.

Das VCS verwaltet:

- Commit-Historie
- Branches
- Blobs
- Trees

Die Cloud kennt keine Commits.

Das VCS kennt die Cloud lediglich als Speicher für Dateien und Objekte.

---

# Zielsetzung

Das Projekt soll sich an der Philosophie des Amiga orientieren:

- klein
- schnell
- verständlich
- ressourcenschonend

Lieber wenige Funktionen, die zuverlässig arbeiten,
als eine vollständige Git-Kompatibilität mit unnötiger Komplexität.

Ein Entwickler soll das gesamte System verstehen können, ohne die interne Komplexität von Git nachbilden zu müssen.

Die Architektur muss jedoch offen genug bleiben, um spätere Erweiterungen (Tags, Revert, Garbage Collection usw.) zu ermöglichen.