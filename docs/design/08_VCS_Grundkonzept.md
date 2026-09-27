# VCS (Cloud Version Control)

## Ziel

Das VCS baut vollständig auf der bestehenden Cloud-Architektur auf.

Es ersetzt die Cloud nicht.

Es erweitert sie.

---

# Grundidee

Cloud:

stellt den aktuellen Dateistand bereit.

VCS:

verwaltet die Historie.

---

# Repository

Ein Repository besteht aus:

- Dateien
- Commits
- Branches

Die Dateien liegen weiterhin in der Cloud.

Zusätzlich existiert eine Commit-Datenbank.

---

# Commit

Ein Commit enthält:

- Commit-ID
- Eltern-Commit
- Zeitstempel
- Autor
- Kommentar
- Dateiliste

Jede Datei verweist auf ihre aktuelle Blob-Version.

---

# Blob

Ein Blob beschreibt einen konkreten Dateiinhalt.

Blob:

- Blob-ID (MD5)
- Größe
- Inhalt

Existiert derselbe Inhalt mehrfach, wird nur ein Blob gespeichert.

---

# Tree

Ein Tree beschreibt:

- Verzeichnisstruktur
- Dateinamen
- Blob-Zuordnung

Dadurch müssen bei einem Commit nicht alle Dateien erneut gespeichert werden.

---

# Branch

Ein Branch verweist lediglich auf einen Commit.

Beispiele:

main

develop

feature/cloud

---

# Checkout

Checkout erzeugt lokal den Zustand eines Commits.

---

# Switch

Switch verändert lediglich den aktuellen Branch.

---

# Merge

Merge erfolgt grundsätzlich clientseitig.

Der Server führt keine Merge-Operationen durch.

Bei Konflikten entscheidet ausschließlich der Client.

---

# Architektur

Cloud:

liefert Dateien.

VCS:

liefert Historie.

Beide Systeme bleiben getrennt.

---

# Wiederverwendung

Das VCS verwendet:

- libcloudhttp
- libcloudfs
- libcloudcore

Später zusätzlich:

- libcloudvcs

---

# Erste Version

Version 1 unterstützt ausschließlich:

- init
- commit
- checkout
- switch
- branch

Merge kann zunächst einfach gehalten oder auf Fast-Forward beschränkt werden.

---

# Langfristige Ziele

- Merge
- Tags
- Historie
- Diff
- Revert
- Garbage Collection

Diese Funktionen sollen auf derselben Architektur aufbauen.