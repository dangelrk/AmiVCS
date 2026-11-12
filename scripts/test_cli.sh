#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# CLI-Test ohne Server (make test): Ausgabe fuer Programme
# (--porcelain-out), Argumente aus Dateien, commit -F, unstage, Loeschen
# bereitstellen, unversionierte Dateien bei switch/merge, Sperrdatei,
# Zeilen-Merge mit Konfliktmarkern, merge --abort, resolve, diff/show.
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

echo "== Merge mit Konflikten"
mkdir "$T/m" && cd "$T/m" || exit 1
"$V" init >/dev/null
printf '1\n2\n3\n4\n5\n6\n7\n8\n' > t.txt; printf 'b\000base' > b.bin; printf 'k\n' > k.txt; printf 'g\n' > gone.txt
v add -A; v commit -m base
v branch feat; v switch feat
printf '1\nTHEIRS\n3\n4\n5\n6\n7\nT8\n' > t.txt; printf 'b\000theirs' > b.bin; printf 'neu\n' > neu.txt
mkdir sub; printf 's\n' > sub/s.txt; printf 'g2\n' > gone.txt
v add -A; v commit -m feat
v switch main
printf '1\nOURS\n3\n4\n5\n6\n7\n8\n' > t.txt; printf 'b\000ours' > b.bin; rm gone.txt
v add -A; v commit -m ours
printf 'meins\n' > meins.txt
v merge feat
chk "merge meldet Konflikt" sh -c "tail -1 '$O' | grep -qx 'end	1'"
chk "Konfliktmarker in t.txt" sh -c "grep -qx '<<<<<<< ours (main)' t.txt && grep -qx '>>>>>>> theirs (feat)' t.txt && grep -qx 'T8' t.txt"
chk "Binaerkonflikt mit .theirs" test -f b.bin.theirs
chk "geloescht/geaendert: Datei wieder da" grep -qx g2 gone.txt
v status
chk "status: merging" has 'merging\tfeat\t'
chk "status: Konflikt content" has 'conflict\tU\tcontent\tt.txt\t0'
chk "status: Konflikt binary" has 'conflict\tU\tbinary\tb.bin\t1'
chk "status: delete-modify" has 'conflict\tU\tdelete-modify\tgone.txt\t0'
chk ".theirs nicht als neue Datei" sh -c "! grep -qF 'b.bin.theirs' '$O'"
v commit -m x
chk "commit mit offenen Konflikten verweigert" has 'unresolved conflict'
v switch feat
chk "switch waehrend Merge verweigert" has 'a merge is in progress'
v merge feat
chk "zweiter merge verweigert" has 'a merge is in progress'
v diff t.txt
chk "diff zeigt Marker" has 'line\t+\t<<<<<<< ours (main)\t1'

echo "== merge --abort"
v merge --abort
chk "abort" sh -c "tail -1 '$O' | grep -qx 'end	0'"
chk "t.txt wie vorher" sh -c "grep -qx OURS t.txt && ! grep -q '<<<<' t.txt"
chk "neue Dateien von theirs weg" sh -c "! test -e neu.txt && ! test -e sub"
chk ".theirs weg" sh -c "! test -e b.bin.theirs"
chk "geloeschte Datei wieder weg" sh -c "! test -e gone.txt"
chk "unversionierte Datei bleibt" grep -qx meins meins.txt
v status
chk "kein Merge mehr" sh -c "! grep -q '^merging' '$O' && ! grep -q 'change	S' '$O'"
v merge --abort
chk "abort ohne Merge" has 'no merge in progress'

echo "== aufloesen und committen"
v merge feat
v resolve --theirs b.bin
chk "resolve --theirs" sh -c "tail -1 '$O' | grep -qx 'end	0' && ! test -e b.bin.theirs"
v resolve --ours gone.txt
chk "resolve --ours (geloescht)" sh -c "! test -e gone.txt"
printf '1\nBEIDE\n3\n4\n5\n6\n7\nT8\n' > t.txt
v add t.txt
v status
chk "alles geloest" has 'merging\tfeat\t'
chk "keine offenen Konflikte" sh -c "! grep -q 'conflict	U' '$O'"
v commit
chk "commit ohne -m im Merge" has 'committed\t'
v log -n 1
chk "Merge-Commit nach Aufloesung hat zwei Eltern" sh -c "grep \"Merge branch 'feat'\" '$O' | cut -f3 | grep -q ','"
v merge feat
chk "erneuter merge: nichts zu tun" has 'Already up to date.'
chk "b.bin von theirs" sh -c "grep -q theirs b.bin"

echo "== Zeilen-Merge ohne Konflikt"
v switch feat; printf '1\nBEIDE\n3\n4\n5\n6\n7\nT8\nende\n' > t.txt; v commit -a -m t2
v switch main; printf 'anfang\n1\nBEIDE\n3\n4\n5\n6\n7\nT8\n' > t.txt; v commit -a -m o2
v merge feat
chk "sauberer Zeilen-Merge committet" has 'Merge completed.'
chk "beide Aenderungen drin" sh -c "head -1 t.txt | grep -qx anfang && tail -1 t.txt | grep -qx ende"

echo "== diff und show"
printf 'x\n' >> k.txt
v diff
chk "diff Arbeitsverzeichnis" has 'file\tM\tk.txt'
chk "diff Zeile" has 'line\t+\tx\t1'
v diff --staged
chk "diff --staged leer" sh -c "! grep -q '^file' '$O'"
v add k.txt; v diff --staged
chk "diff --staged" has 'file\tM\tk.txt'
v show HEAD
chk "show: Commit" has "Merge branch 'feat'"
chk "show: Datei" has 'file\tM\tt.txt'
v show HEAD~1
chk "show HEAD~1" has 'commit\t'
"$V" diff HEAD~2 HEAD -- t.txt > "$T/d.txt" 2>&1
chk "diff zwischen Commits (Text)" sh -c "grep -qx -- '--- a/t.txt' '$T/d.txt' && grep -qx '+anfang' '$T/d.txt'"
v show zzzz
chk "unbekannte Revision" has 'unknown revision'

echo "== Befunde der Pruefung"
mkdir "$T/p" && cd "$T/p" || exit 1
"$V" init >/dev/null
printf '1\n2\n3\n' > t.txt; printf 'b\000base' > b.bin; mkdir d; printf 'd\000base' > d/x.bin; printf 'g\n' > g.txt
v add -A; v commit -m base
v branch f; v switch f
printf '1\nF\n3\n' > t.txt; printf 'b\000f' > b.bin; printf 'd\000f' > d/x.bin; printf 'g2\n' > g.txt
v commit -a -m f
v switch main
printf '1\nM\n3\n' > t.txt; printf 'b\000m' > b.bin; printf 'd\000m' > d/x.bin; rm g.txt
v commit -a -m m
printf 'MEINE NOTIZEN\n' > b.bin.theirs
v merge f
chk "fremde .theirs-Datei: Merge verweigert" has 'b.bin.theirs already exists'
chk "fremde .theirs-Datei unveraendert" grep -qx 'MEINE NOTIZEN' b.bin.theirs
chk "nichts geschrieben" sh -c "! test -e .amivcs/MERGE_HEAD && grep -qx M t.txt"
rm b.bin.theirs
v merge f
chk "Merge mit Konflikten" has 'conflict\t'
v commit -a -m x
chk "commit -a mit offenen Konflikten verweigert" has 'commit -a would commit the conflict'
chk "Marker nicht committet" sh -c "test -f .amivcs/MERGE_HEAD"
v add d
v status
chk "add <dir> merkt .theirs nicht vor" sh -c "! grep -q 'x.bin.theirs' '$O'"
chk "add <dir> loest d/x.bin" has 'conflict\tR\tbinary\td/x.bin'
v add g.txt
chk "add auf geloeschte Konfliktdatei" sh -c "tail -1 '$O' | grep -qx 'end	0'"
v status
chk "geloeschte Konfliktdatei geloest" has 'conflict\tR\tdelete-modify\tg.txt'
v merge --abort
printf 'neu\n' > staged.txt; v add staged.txt; printf 'x\n' > untracked.txt
v diff HEAD
chk "diff HEAD zeigt vorgemerkte neue Datei" has 'file\tN\tstaged.txt'
v diff .
chk "diff . ohne unversionierte Dateien" sh -c "! grep -q 'untracked.txt' '$O'"
v diff untracked.txt
chk "diff <neue Datei> zeigt sie" has 'file\tN\tuntracked.txt'
v diff ./staged.txt
chk "diff ./pfad" sh -c "tail -1 '$O' | grep -qx 'end	0'"
rm -f t.txt
v diff t.txt
chk "diff auf geloeschte Datei" has 'file\tD\tt.txt'
v diff f -- d/x.bin
chk "diff <branch> -- <pfad>" has 'file\tM\td/x.bin'

echo "== gc: Pack-Namen passen in 30 Zeichen, alte pack-<32hex> bleiben lesbar"
mkdir "$T/g" && cd "$T/g" || exit 1
"$V" init >/dev/null
printf 'eins\n' > eins.txt; "$V" add eins.txt >/dev/null; "$V" commit -m eins >/dev/null
"$V" gc >/dev/null 2>&1
P=$(ls .amivcs/objects/pack/*.pack 2>/dev/null | head -1)
chk "gc: Pack-Name <= 30 Zeichen" sh -c "[ -n '$P' ] && [ \$(basename '$P' | wc -c) -le 31 ]"
chk "gc: idx neben pack" sh -c "[ -f '${P%.pack}.idx' ]"
# altes Format nachstellen: pack-<32hex>.pack/.idx
B=$(basename "$P" .pack)
mv "$P" ".amivcs/objects/pack/pack-${B}deadbeef.pack"
mv ".amivcs/objects/pack/$B.idx" ".amivcs/objects/pack/pack-${B}deadbeef.idx"
printf 'zwei\n' > zwei.txt; "$V" add zwei.txt >/dev/null; "$V" commit -m zwei >/dev/null
"$V" gc >/dev/null 2>&1
chk "gc: zweites Pack im neuen Format" sh -c "ls .amivcs/objects/pack | grep -qv '^pack-'"
chk "gc: keine Loose-Objekte mehr" sh -c "[ -z \"\$(find .amivcs/objects -path '*/pack' -prune -o -type f -print)\" ]"
"$V" branch behalten >/dev/null 2>&1
rm -f eins.txt zwei.txt; "$V" add eins.txt zwei.txt >/dev/null 2>&1; "$V" commit -m weg >/dev/null 2>&1
chk "Dateien entfernt" sh -c "[ ! -f eins.txt ] && [ ! -f zwei.txt ]"
"$V" switch behalten >/dev/null 2>&1
chk "switch holt Dateien aus altem und neuem Pack" sh -c "grep -qx eins eins.txt && grep -qx zwei zwei.txt"
v log -n 5
chk "log liest beide Packs" sh -c "grep -q 'eins' '$O' && grep -q 'zwei' '$O'"

echo "== Namenslaenge: nichts schreiben, was das Dateisystem kuerzen wuerde"
mkdir "$T/n" && cd "$T/n" || exit 1
"$V" init >/dev/null
"$V" config longnames on >/dev/null 2>&1   # Projekt vom PC mit langen Namen
LANG30=abcdefghijklmnopqrstuvwxyz_1.c     # genau 30 Zeichen
LANG31=abcdefghijklmnopqrstuvwxyz_12.c    # 31 Zeichen
BIN30=binaerdatei_genau_dreissig.bin   # 30 Zeichen, binaer -> .theirs statt Marker
printf 'basis\n' > "$LANG30"; printf 'x\000basis' > "$BIN30"
"$V" add "$LANG30" "$BIN30" >/dev/null; "$V" commit -m basis >/dev/null
"$V" branch lang >/dev/null 2>&1; "$V" switch lang >/dev/null 2>&1
printf 'lang\n' > "$LANG31"; "$V" add "$LANG31" >/dev/null 2>&1
printf 'theirs\n' > "$LANG30"; printf 'x\000theirs' > "$BIN30"; "$V" add "$LANG30" "$BIN30" >/dev/null 2>&1
"$V" commit -m lang >/dev/null 2>&1
"$V" switch main >/dev/null 2>&1
chk "switch auf main ohne lange Datei" sh -c "[ ! -f '$LANG31' ]"
AMICLOUD_NAME_MAX=30 "$V" switch lang >/dev/null 2>"$T/err"
chk "switch mit 31-Zeichen-Namen auf FFS abgelehnt" sh -c "grep -q 'longer than this file system keeps' '$T/err'"
chk "  ... und nichts geaendert" sh -c "[ ! -f '$LANG31' ] && grep -qx basis '$LANG30'"
printf 'ours\n' > "$LANG30"; "$V" add "$LANG30" >/dev/null; "$V" commit -m ours >/dev/null
AMICLOUD_NAME_MAX=30 "$V" merge lang >/dev/null 2>"$T/err"
chk "merge mit 31-Zeichen-Namen auf FFS abgelehnt" sh -c "grep -q 'longer than this file system keeps' '$T/err' && grep -qx ours '$LANG30'"
# nur der 30-Zeichen-Konflikt: .theirs muss gekuerzt werden
"$V" switch lang >/dev/null 2>&1; rm -f "$LANG31"; "$V" add "$LANG31" >/dev/null 2>&1; "$V" commit -m kurz >/dev/null 2>&1
"$V" switch main >/dev/null 2>&1
printf 'x\000ours' > "$BIN30"; "$V" add "$BIN30" >/dev/null; "$V" commit -m binours >/dev/null
AMICLOUD_NAME_MAX=30 "$V" merge lang >/dev/null 2>&1
chk "Konflikt: Arbeitsdatei behaelt ours" sh -c "grep -qa 'ours' '$BIN30' && ! grep -qa theirs '$BIN30'"
TH=$(ls | grep '\.theirs$' | head -1)
chk "Konflikt: .theirs-Name passt in 30 Zeichen" sh -c "[ -n '$TH' ] && [ \$(printf %s '$TH' | wc -c) -le 30 ] && grep -qa theirs '$TH'"
AMICLOUD_NAME_MAX=30 "$V" merge --abort >/dev/null 2>&1
chk "merge --abort raeumt gekuerzte .theirs weg" sh -c "! ls | grep -q '\.theirs$'"
AMICLOUD_NAME_MAX=30 "$V" merge lang >/dev/null 2>&1
AMICLOUD_NAME_MAX=30 "$V" resolve --theirs "$BIN30" >/dev/null 2>&1
chk "resolve raeumt gekuerzte .theirs weg" sh -c "! ls | grep -q '\.theirs$' && grep -qa theirs '$BIN30'"
"$V" add "$LANG30" >/dev/null 2>&1; AMICLOUD_NAME_MAX=30 "$V" commit -m merged >/dev/null 2>&1
chk "commit nach gekuerztem Konflikt" sh -c "[ ! -f .amivcs/MERGE_HEAD ]"

echo "== Amiga-Modus (Standard): neue Namen muessen auf FFS passen"
mkdir "$T/a" && cd "$T/a" || exit 1
"$V" init >/dev/null
printf 'x\n' > abcdefghijklmnopqrstuvwxyz_12.c
"$V" add abcdefghijklmnopqrstuvwxyz_12.c >/dev/null 2>&1
"$V" commit -m lang >/dev/null 2>"$T/err"
chk "31 Zeichen abgelehnt" sh -c "grep -q 'more than 30 characters' '$T/err' && grep -q 'longnames on' '$T/err'"
"$V" unstage abcdefghijklmnopqrstuvwxyz_12.c >/dev/null 2>&1; rm -f abcdefghijklmnopqrstuvwxyz_12.c
printf 'x\n' > abcdefghijklmnopqrstuvwxyz_1.c; "$V" add abcdefghijklmnopqrstuvwxyz_1.c >/dev/null 2>&1
"$V" commit -m genau30 >/dev/null 2>&1
chk "30 Zeichen erlaubt" sh -c "\"$V\" log -n 1 | grep -q genau30"
printf 'x\n' > 'a:b.txt'; "$V" add 'a:b.txt' >/dev/null 2>&1
"$V" commit -m doppelpunkt >/dev/null 2>"$T/err"
chk "Doppelpunkt abgelehnt" grep -q "':' is not allowed" "$T/err"
"$V" unstage 'a:b.txt' >/dev/null 2>&1; rm -f 'a:b.txt'
printf 'x\n' > Readme.txt; "$V" add Readme.txt >/dev/null 2>&1; "$V" commit -m readme >/dev/null 2>&1
if [ "$(uname)" = Linux ]; then
  printf 'y\n' > README.txt; "$V" add README.txt >/dev/null 2>&1
  "$V" commit -m gross >/dev/null 2>"$T/err"
  chk "nur Gross/Klein verschieden abgelehnt" grep -q 'differ only in case' "$T/err"
  "$V" unstage README.txt >/dev/null 2>&1; rm -f README.txt
fi
mkdir -p ganz_langer_ordnername_mit_32_zeichen
printf 'x\n' > ganz_langer_ordnername_mit_32_zeichen/a.txt
"$V" add ganz_langer_ordnername_mit_32_zeichen >/dev/null 2>&1
"$V" commit -m ordner >/dev/null 2>"$T/err"
chk "langer neuer Ordner abgelehnt" grep -q 'more than 30 characters' "$T/err"
"$V" config longnames on >/dev/null 2>&1
"$V" commit -m ordner >/dev/null 2>&1
chk "mit longnames on erlaubt" sh -c "\"$V\" log -n 1 | grep -q ordner"
"$V" config longnames off >/dev/null 2>&1
printf 'y\n' > ganz_langer_ordnername_mit_32_zeichen/b.txt
printf 'z\n' > ganz_langer_ordnername_mit_32_zeichen/a.txt
"$V" add ganz_langer_ordnername_mit_32_zeichen >/dev/null 2>&1
"$V" commit -m bestand >/dev/null 2>"$T/err"
chk "Bestand: neue Datei im vorhandenen langen Ordner erlaubt" sh -c "\"$V\" log -n 1 | grep -q bestand"
chk "config longnames zeigt off" sh -c "[ \"\$(\"$V\" config longnames)\" = off ]"
chk ".amivcsproject angelegt" grep -qx 'longnames=0' .amivcsproject

echo "== Ergebnis: $PASSED ok, $FAILED fehlgeschlagen =="
[ "$FAILED" -eq 0 ]
