#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# CLI-Test ohne Server (make test): Ausgabe fuer Programme
# (--porcelain-out), Argumente aus Dateien, commit -F, unstage, Loeschen
# bereitstellen, unversionierte Dateien bei switch/merge, Sperrdatei.
# Braucht bin/pc/amivcs (make pc).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
V="$ROOT/bin/pc/amivcs"
[ -x "$V" ] || { echo "$V fehlt (make pc)" >&2; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
O="$T/out.pc"
# globale Vorgabe (Autor) in einem eigenen HOME
export HOME="$T/home"
mkdir -p "$HOME"
printf 'name=Tester\n' > "$HOME/.amivcsconfig"
PASSED=0; FAILED=0
ok()  { PASSED=$((PASSED + 1)); echo "ok    $1"; }
bad() { FAILED=$((FAILED + 1)); echo "FEHLER $1"; [ -f "$O" ] && sed 's/^/      /' "$O"; }
v()   { "$V" --porcelain-out "$O" "$@" >/dev/null 2>&1; }
has() { grep -qF -- "$(printf "$1")" "$O"; }
chk() { name=$1; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }

mkdir "$T/r" && cd "$T/r" || exit 1
"$V" init >/dev/null
printf 'a\n' > a.txt; printf 'b\n' > b.txt; mkdir d; printf 'x\n' > d/x.txt
printf 'x' > "$(printf 'tab\tname.txt')"

echo "== Format"
v version; chk "format und end" sh -c "head -1 '$O' | grep -qx 'format	1' && tail -1 '$O' | grep -qx 'end	0'"

echo "== Argumente aus Datei, Maskierung"
printf 'a.txt\nb.txt\nd\ntab\\tname.txt\n' > "$T/args"
v --args-from "$T/args" add
chk "add ueber --args-from" has 'change\tS\tN\ta.txt'
chk "Tab im Namen maskiert" has 'change\tS\tN\ttab\\tname.txt'
chk "Status nach add angehaengt" has 'branch\tmain'

echo "== commit -F"
printf 'Erste Zeile\nzweite mit "Quote" ; $x\n' > "$T/msg"
v commit -F "$T/msg"
chk "commit -F" has 'committed\t'
v log --all -n 5
chk "Nachricht maskiert" has 'Erste Zeile\\nzweite mit "Quote" ; $x'
chk "Autor aus Konfiguration" has '\tTester\t'

echo "== unstage"
printf 'a2\n' > a.txt; v add a.txt; v unstage a.txt
chk "unstage einer geaenderten Datei" sh -c "grep -qF \"\$(printf 'change\tU\tM\ta.txt')\" '$O' && ! grep -q 'change	S' '$O'"
printf 'n\n' > n.txt; v add n.txt; v unstage n.txt
chk "unstage einer neuen Datei" has 'change\tU\tN\tn.txt'
"$V" checkout >/dev/null 2>&1 || true
printf 'a\n' > a.txt

echo "== Loeschen bereitstellen"
rm b.txt; v add b.txt
chk "add auf geloeschte Datei" has 'change\tS\tD\tb.txt'
rm d/x.txt; v add d
chk "add auf Verzeichnis mit geloeschter Datei" has 'change\tS\tD\td/x.txt'
v commit -F "$T/msg"; chk "commit der Loeschungen" has 'committed\t'

echo "== unversionierte Dateien"
v branch dev
chk "branch anlegen" has 'ref\t-\tdev'
printf 'frei\n' > n.txt
v switch dev; chk "switch trotz unversionierter Datei" sh -c "tail -1 '$O' | grep -qx 'end	0'"
chk "unversionierte Datei bleibt" test -f n.txt
v switch main; printf 'drin\n' > n.txt; v add n.txt; v commit -F "$T/msg"
v switch dev; printf 'anders\n' > n.txt
v switch main
chk "ueberschreiben verweigert" has 'msg\terror\tamivcs: n.txt is not under version control and would be overwritten'
chk "Datei unveraendert" grep -qx anders n.txt
rm n.txt; v switch main
printf 'dev\n' > dev.txt; v switch dev; printf 'dv\n' > dv.txt; v add dv.txt; v commit -F "$T/msg"
v switch main; printf 'fremd\n' > fremd.txt
v merge dev
chk "merge" sh -c "tail -1 '$O' | grep -qx 'end	0'"
v log --all
chk "Merge-Commit hat zwei Eltern" sh -c "grep \"Merge branch 'dev'\" '$O' | cut -f3 | grep -q ','"
chk "Merge-Commit traegt die Identitaet" sh -c "grep \"Merge branch 'dev'\" '$O' | cut -f5 | grep -qx 'Tester'"
v status
chk "fremde Datei nicht eingesammelt" has 'change\tU\tN\tfremd.txt'

echo "== Sperrdatei"
: > .amivcs/lock
v add fremd.txt
chk "zweiter Schreiber abgewiesen" has 'msg\terror\tamivcs: another program'
v status; chk "Lesen geht trotz Sperre" sh -c "tail -1 '$O' | grep -qx 'end	0'"
rm .amivcs/lock

echo "== Ergebnis: $PASSED ok, $FAILED fehlgeschlagen =="
[ "$FAILED" -eq 0 ]
